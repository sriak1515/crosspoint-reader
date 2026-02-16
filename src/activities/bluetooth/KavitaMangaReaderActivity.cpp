/**
 * KavitaMangaReaderActivity.cpp
 *
 * BLE-based manga reader activity that receives XTH 2-bit format pages
 * from a companion app and displays them on the e-ink display.
 */

#include "KavitaMangaReaderActivity.h"

#include <NimBLEDevice.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"
#include "components/UITheme.h"
#include "fontIds.h"

// Protocol commands (sent via cmd characteristic)
enum class BleCommand : uint8_t {
  REQUEST_LIST = 0x01,      // Request manga list from app
  REQUEST_PAGE = 0x02,      // Request specific page (mangaId + pageNumber)
  ACKNOWLEDGE = 0x03,       // Acknowledge data received
  CANCEL_TRANSFER = 0x04,   // Cancel current transfer
  DISCONNECT = 0x05         // Graceful disconnect
};

// Response status from companion app
enum class BleStatus : uint8_t {
  OK = 0x00,
  ERROR = 0x01,
  LIST_START = 0x10,        // Start of manga list transfer
  LIST_ENTRY = 0x11,        // Single manga entry
  LIST_END = 0x12,          // End of manga list
  PAGE_START = 0x20,        // Start of page transfer
  PAGE_DATA = 0x21,         // Page data chunk
  PAGE_END = 0x22           // End of page transfer
};

// Buffer for receiving page data
// XTH 2-bit format: ((width * height + 7) / 8) * 2 bytes
// For 480x800 display: ((480 * 800 + 7) / 8) * 2 = 96,000 bytes
static constexpr size_t MAX_PAGE_BUFFER_SIZE = ((480 * 800 + 7) / 8) * 2;
static constexpr uint16_t DISPLAY_WIDTH = 480;
static constexpr uint16_t DISPLAY_HEIGHT = 800;

// ============================================================================
// Constructor & Destructor
// ============================================================================

KavitaMangaReaderActivity::KavitaMangaReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const std::function<void()>& onGoHome)
    : ActivityWithSubactivity("KavitaMangaReader", renderer, mappedInput),
      onGoHome(onGoHome),
      serverCallbacks(this),
      requestCallbacks(this) {}

KavitaMangaReaderActivity::~KavitaMangaReaderActivity() {
  stopAdvertising();
  if (bleServer) {
    NimBLEDevice::deinit();
  }
}

// ============================================================================
// Activity Lifecycle
// ============================================================================

void KavitaMangaReaderActivity::onEnter() {
  LOG_INF("KMR", "Entering Kavita Manga Reader");
  
  // Initialize state
  state = KavitaMangaReaderState::CHECK_COMPANION_APP;
  errorMessage.clear();
  mangaList.clear();
  currentMangaIndex = -1;
  
  // Setup BLE
  setupBLE();
  startAdvertising();
  
  requestUpdate();
}

void KavitaMangaReaderActivity::onExit() {
  LOG_INF("KMR", "Exiting Kavita Manga Reader");
  
  stopAdvertising();
  mangaList.clear();
}

void KavitaMangaReaderActivity::loop() {
  // Call parent loop for subactivity handling
  ActivityWithSubactivity::loop();
  
  // State machine dispatch
  switch (state) {
    case KavitaMangaReaderState::CHECK_COMPANION_APP:
      handleCheckCompanionApp();
      break;
    case KavitaMangaReaderState::WAITING_FOR_APP:
      handleWaitForCompanionApp();
      break;
    case KavitaMangaReaderState::LOAD_LIST:
      handleLoadList();
      break;
    case KavitaMangaReaderState::RECEIVING_LIST:
      handleReceiveList();
      break;
    case KavitaMangaReaderState::BROWSING_LIST:
      handleBrowsingList();
      break;
    case KavitaMangaReaderState::LOAD_PAGE:
      handleLoadPage();
      break;
    case KavitaMangaReaderState::RECEIVING_PAGE:
      handleReceivingPage();
      break;
    case KavitaMangaReaderState::DISPLAY_PAGE:
      handleDisplayPage();
      break;
    case KavitaMangaReaderState::ERROR:
      handleError();
      break;
  }
}

void KavitaMangaReaderActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();
  
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  auto metrics = UITheme::getInstance().getMetrics();
  
  switch (state) {
    case KavitaMangaReaderState::CHECK_COMPANION_APP:
    case KavitaMangaReaderState::WAITING_FOR_APP:
      // Draw waiting screen
      renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 30, "Kavita Manga Reader", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "Waiting for companion app...", true, EpdFontFamily::REGULAR);
      
      if (bleServer && bleServer->getConnectedCount() > 0) {
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, "Connected!", true, EpdFontFamily::REGULAR);
      } else {
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, "Advertising...", true, EpdFontFamily::REGULAR);
      }
      break;
      
    case KavitaMangaReaderState::LOAD_LIST:
    case KavitaMangaReaderState::RECEIVING_LIST:
      // Draw loading screen
      renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, "Loading Library", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, "Receiving manga list...", true, EpdFontFamily::REGULAR);
      break;
      
    case KavitaMangaReaderState::BROWSING_LIST: {
      // Draw manga list
      GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "Kavita Library");
      
      const int contentY = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
      const int contentHeight = pageHeight - contentY - metrics.buttonHintsHeight;
      const int itemHeight = metrics.listRowHeight;
      const int visibleItems = contentHeight / itemHeight;
      
      if (mangaList.empty()) {
        renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2, "No manga found", true, EpdFontFamily::REGULAR);
      } else {
        // Draw list items
        int listOffset = std::max(0, currentMangaIndex - visibleItems / 2);
        
        for (int i = 0; i < visibleItems && (listOffset + i) < static_cast<int>(mangaList.size()); i++) {
          const int itemY = contentY + i * itemHeight;
          const int itemIndex = listOffset + i;
          const bool isSelected = (itemIndex == currentMangaIndex);
          
          // Draw selection highlight
          if (isSelected) {
            renderer.fillRect(metrics.contentSidePadding, itemY, pageWidth - 2 * metrics.contentSidePadding, itemHeight - 2, true);
          }
          
          // Draw title
          const char* textColor = isSelected ? "Selected" : "Normal";
          renderer.drawText(UI_12_FONT_ID, metrics.contentSidePadding + 5, itemY + itemHeight / 2,
                           mangaList[itemIndex].title.c_str(), !isSelected, EpdFontFamily::REGULAR);
        }
        
        // Draw page indicator
        if (mangaList.size() > 1) {
          char indicator[32];
          snprintf(indicator, sizeof(indicator), "%d / %zu", currentMangaIndex + 1, mangaList.size());
          renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - metrics.buttonHintsHeight - 20, indicator, true, EpdFontFamily::REGULAR);
        }
      }
      
      // Draw button hints
      const auto labels = mappedInput.mapLabels("Back", "Select", "Up", "Down");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      break;
    }
    
    case KavitaMangaReaderState::LOAD_PAGE:
    case KavitaMangaReaderState::RECEIVING_PAGE:
      // Draw loading screen
      renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, "Loading Page", true, EpdFontFamily::BOLD);
      if (!mangaList.empty() && currentMangaIndex >= 0) {
        renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, 
                                 mangaList[currentMangaIndex].title.c_str(), true, EpdFontFamily::REGULAR);
      }
      break;
      
    case KavitaMangaReaderState::DISPLAY_PAGE:
      // Page is rendered separately in handleDisplayPage
      // Just show button hints
      {
        const auto labels = mappedInput.mapLabels("Back", "", "Prev", "Next");
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      }
      break;
      
    case KavitaMangaReaderState::ERROR:
      // Draw error screen
      renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 - 20, "Error", true, EpdFontFamily::BOLD);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str(), true, EpdFontFamily::REGULAR);
      
      {
        const auto labels = mappedInput.mapLabels("Back", "", "", "");
        GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
      }
      break;
  }
  
  renderer.displayBuffer();
}

// ============================================================================
// BLE Setup
// ============================================================================

void KavitaMangaReaderActivity::setupBLE() {
  LOG_DBG("KMR", "Setting up BLE");
  
  // Initialize NimBLE
  NimBLEDevice::init("CrossPoint Reader");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // Maximum power for better range
  
  // Create BLE Server
  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(&serverCallbacks);
  
  // Create Kavita Service
  kavitaService = bleServer->createService(KAVITA_SERVICE_UUID);
  
  // Create Command Characteristic (write from app)
  cmdCharacteristic = kavitaService->createCharacteristic(
      KAVITA_CMD_CHAR_UUID,
      NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR
  );
  cmdCharacteristic->setCallbacks(&requestCallbacks);
  
  // Create Data Characteristic (notify/read from app)
  dataCharacteristic = kavitaService->createCharacteristic(
      KAVITA_DATA_CHAR_UUID,
      NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
  );
  
  // Start the service
  kavitaService->start();
  
  LOG_DBG("KMR", "BLE setup complete");
}

void KavitaMangaReaderActivity::startAdvertising() {
  LOG_DBG("KMR", "Starting BLE advertising");
  
  NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
  
  // Set up advertisement data
  NimBLEAdvertisementData advData;
  advData.setName("CrossPoint Reader");
  advData.setCompleteServices(NimBLEUUID(KAVITA_SERVICE_UUID));
  pAdvertising->setAdvertisementData(advData);
  
  // Set scan response data
  NimBLEAdvertisementData scanData;
  scanData.setCompleteServices(NimBLEUUID(KAVITA_SERVICE_UUID));
  pAdvertising->setScanResponseData(scanData);
  
  // Start advertising
  pAdvertising->start();
}

void KavitaMangaReaderActivity::stopAdvertising() {
  LOG_DBG("KMR", "Stopping BLE advertising");
  
  if (bleServer) {
    NimBLEDevice::getAdvertising()->stop();
  }
}

// ============================================================================
// State Machine Handlers
// ============================================================================

void KavitaMangaReaderActivity::handleCheckCompanionApp() {
  LOG_DBG("KMR", "Checking for companion app connection");
  
  if (bleServer && bleServer->getConnectedCount() > 0) {
    LOG_INF("KMR", "Companion app connected");
    state = KavitaMangaReaderState::LOAD_LIST;
  } else {
    state = KavitaMangaReaderState::WAITING_FOR_APP;
  }
  
  requestUpdate();
}

void KavitaMangaReaderActivity::handleWaitForCompanionApp() {
  // Check if companion app connected
  if (bleServer && bleServer->getConnectedCount() > 0) {
    LOG_INF("KMR", "Companion app connected, requesting manga list");
    state = KavitaMangaReaderState::LOAD_LIST;
    requestUpdate();
  }
  // Continue waiting - onConnect callback will trigger state change
}

void KavitaMangaReaderActivity::handleLoadList() {
  LOG_DBG("KMR", "Requesting manga list from companion app");
  
  // Clear existing list
  mangaList.clear();
  
  // Send REQUEST_LIST command
  if (cmdCharacteristic) {
    uint8_t cmd = static_cast<uint8_t>(BleCommand::REQUEST_LIST);
    dataCharacteristic->setValue(&cmd, 1);
    dataCharacteristic->notify();
    
    state = KavitaMangaReaderState::RECEIVING_LIST;
    requestUpdate();
  } else {
    errorMessage = "BLE not initialized";
    state = KavitaMangaReaderState::ERROR;
    requestUpdate();
  }
}

void KavitaMangaReaderActivity::handleReceiveList() {
  // This state is handled by the BLE callback (onWrite)
  // We just wait here until the list is complete
  // Timeout handling could be added here
}

void KavitaMangaReaderActivity::handleBrowsingList() {
  // Handle input for browsing
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onGoHome();
    return;
  }
  
  if (mappedInput.wasPressed(MappedInputManager::Button::Up)) {
    if (currentMangaIndex > 0) {
      currentMangaIndex--;
      requestUpdate();
    }
  }
  
  if (mappedInput.wasPressed(MappedInputManager::Button::Down)) {
    if (currentMangaIndex < static_cast<int>(mangaList.size()) - 1) {
      currentMangaIndex++;
      requestUpdate();
    }
  }
  
  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    if (currentMangaIndex >= 0 && currentMangaIndex < static_cast<int>(mangaList.size())) {
      state = KavitaMangaReaderState::LOAD_PAGE;
      requestUpdate();
    }
  }
}

void KavitaMangaReaderActivity::handleLoadPage() {
  LOG_DBG("KMR", "Requesting page from companion app");
  
  if (currentMangaIndex < 0 || currentMangaIndex >= static_cast<int>(mangaList.size())) {
    errorMessage = "Invalid manga selection";
    state = KavitaMangaReaderState::ERROR;
    requestUpdate();
    return;
  }
  
  // Send REQUEST_PAGE command with manga ID
  // Format: [CMD][mangaId length][mangaId][page number (2 bytes)]
  std::string mangaId = mangaList[currentMangaIndex].id;
  uint8_t requestBuffer[1 + 1 + mangaId.length() + 2];
  requestBuffer[0] = static_cast<uint8_t>(BleCommand::REQUEST_PAGE);
  requestBuffer[1] = static_cast<uint8_t>(mangaId.length());
  memcpy(&requestBuffer[2], mangaId.c_str(), mangaId.length());
  requestBuffer[2 + mangaId.length()] = 0;  // Page number MSB (TODO: track current page)
  requestBuffer[2 + mangaId.length() + 1] = 0;  // Page number LSB
  
  if (dataCharacteristic) {
    dataCharacteristic->setValue(requestBuffer, sizeof(requestBuffer));
    dataCharacteristic->notify();
    
    state = KavitaMangaReaderState::RECEIVING_PAGE;
    requestUpdate();
  } else {
    errorMessage = "BLE not initialized";
    state = KavitaMangaReaderState::ERROR;
    requestUpdate();
  }
}

void KavitaMangaReaderActivity::handleReceivingPage() {
  // This state is handled by the BLE callback (onWrite)
  // Page data is received in chunks and rendered when complete
}

void KavitaMangaReaderActivity::handleDisplayPage() {
  // Handle navigation while viewing page
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    state = KavitaMangaReaderState::BROWSING_LIST;
    requestUpdate();
    return;
  }
  
  if (mappedInput.wasPressed(MappedInputManager::Button::Left) || 
      mappedInput.wasPressed(MappedInputManager::Button::PageBack)) {
    // TODO: Request previous page
    state = KavitaMangaReaderState::LOAD_PAGE;
    requestUpdate();
  }
  
  if (mappedInput.wasPressed(MappedInputManager::Button::Right) || 
      mappedInput.wasPressed(MappedInputManager::Button::PageForward)) {
    // TODO: Request next page
    state = KavitaMangaReaderState::LOAD_PAGE;
    requestUpdate();
  }
}

void KavitaMangaReaderActivity::handleError() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
      mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    onGoHome();
  }
}

// ============================================================================
// BLE Server Callbacks
// ============================================================================

void KavitaMangaReaderActivity::ServerCallbacks::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
  LOG_INF("KMR", "Companion app connected");
  
  // Update MTU for faster transfers
  pServer->updateConnParams(connInfo.getConnHandle(), 6, 100, 0, 500);
  
  activity->requestUpdate();
}

void KavitaMangaReaderActivity::ServerCallbacks::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
  LOG_INF("KMR", "Companion app disconnected (reason: %d)", reason);
  
  activity->state = KavitaMangaReaderState::WAITING_FOR_APP;
  activity->requestUpdate();
}

// ============================================================================
// BLE Characteristic Callbacks
// ============================================================================

void KavitaMangaReaderActivity::RequestCallbacks::onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
  const uint8_t* data = pCharacteristic->getValue().data();
  const size_t length = pCharacteristic->getLength();
  
  if (length == 0) {
    return;
  }
  
  const uint8_t status = data[0];
  LOG_DBG("KMR", "Received BLE data: status=0x%02X, length=%zu", status, length);
  
  switch (status) {
    case static_cast<uint8_t>(BleStatus::LIST_START):
      LOG_DBG("KMR", "List transfer started");
      activity->mangaList.clear();
      break;
      
    case static_cast<uint8_t>(BleStatus::LIST_ENTRY): {
      // Format: [status][id_length][id...][title_length][title...]
      if (length >= 3) {
        const uint8_t idLen = data[1];
        if (length >= 2 + idLen + 1) {
          std::string id(reinterpret_cast<const char*>(&data[2]), idLen);
          const uint8_t titleLen = data[2 + idLen];
          if (length >= 2 + idLen + 1 + titleLen) {
            std::string title(reinterpret_cast<const char*>(&data[2 + idLen + 1]), titleLen);
            activity->mangaList.push_back({id, title});
            LOG_DBG("KMR", "Added manga: %s", title.c_str());
          }
        }
      }
      break;
    }
    
    case static_cast<uint8_t>(BleStatus::LIST_END):
      LOG_INF("KMR", "List transfer complete (%zu entries)", activity->mangaList.size());
      activity->currentMangaIndex = activity->mangaList.empty() ? -1 : 0;
      activity->state = KavitaMangaReaderState::BROWSING_LIST;
      activity->requestUpdate();
      break;
      
    case static_cast<uint8_t>(BleStatus::PAGE_START):
      LOG_DBG("KMR", "Page transfer started");
      // TODO: Initialize page buffer
      break;
      
    case static_cast<uint8_t>(BleStatus::PAGE_DATA): {
      // Append page data chunk to buffer
      // Format: [status][chunk_offset (4 bytes)][data...]
      if (length > 5) {
        const uint32_t offset = (data[1] << 24) | (data[2] << 16) | (data[3] << 8) | data[4];
        const uint8_t* chunkData = &data[5];
        const size_t chunkSize = length - 5;
        LOG_DBG("KMR", "Page chunk: offset=%u, size=%zu", offset, chunkSize);
        // TODO: Store chunk in page buffer at offset
      }
      break;
    }
    
    case static_cast<uint8_t>(BleStatus::PAGE_END):
      LOG_INF("KMR", "Page transfer complete");
      // TODO: Render the received page
      activity->state = KavitaMangaReaderState::DISPLAY_PAGE;
      activity->requestUpdate();
      break;
      
    case static_cast<uint8_t>(BleStatus::ERROR):
      LOG_ERR("KMR", "Companion app reported error");
      activity->errorMessage = "Companion app error";
      activity->state = KavitaMangaReaderState::ERROR;
      activity->requestUpdate();
      break;
      
    default:
      LOG_DBG("KMR", "Unknown status: 0x%02X", status);
      break;
  }
}