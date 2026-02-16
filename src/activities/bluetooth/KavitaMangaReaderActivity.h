#pragma once
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
#include <SDCardManager.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"

// BLE Service and Characteristic UUIDs
#define KAVITA_SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define KAVITA_CMD_CHAR_UUID "4fafc202-1fb5-459e-8fcc-c5c9c331914b"
#define KAVITA_DATA_CHAR_UUID "4fafc203-1fb5-459e-8fcc-c5c9c331914b"

// Data structures for manga information
struct MangaEntry {
  std::string id;
  std::string title;
};

class KavitaMangaReaderActivity final : public ActivityWithSubactivity {
 public:
  enum class KavitaMangaReaderState {
    // Connection states
    CHECK_COMPANION_APP,  // Checking if companion app is connected
    WAITING_FOR_APP,      // Waiting for companion app to connect

    // List loading states
    LOAD_LIST,       // Requesting manga list from app
    RECEIVING_LIST,  // Receiving chunked list data

    // Browsing states
    BROWSING_LIST,  // Displaying manga entries

    // Page loading states
    LOAD_PAGE,       // Requesting page from app
    RECEIVING_PAGE,  // Receiving chunked page data
    DISPLAY_PAGE,    // Displaying the page

    // Error state
    ERROR  // Error state with message
  };

  explicit KavitaMangaReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::function<void()>& onGoHome);
  ~KavitaMangaReaderActivity();

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&&) override;
  bool preventAutoSleep() override { return true; }  // Keep awake during reading

 private:
  // State management
  KavitaMangaReaderState state = KavitaMangaReaderState::CHECK_COMPANION_APP;
  std::string errorMessage;
  const std::function<void()> onGoHome;

  // BLE components
  NimBLEServer* bleServer = nullptr;
  NimBLEService* kavitaService = nullptr;
  NimBLECharacteristic* cmdCharacteristic = nullptr;
  NimBLECharacteristic* dataCharacteristic = nullptr;

  // Manga data
  std::vector<MangaEntry> mangaList;
  int currentMangaIndex = -1;

  // BLE setup methods
  void setupBLE();
  void startAdvertising();
  void stopAdvertising();

  // State machine handlers
  void handleCheckCompanionApp();
  void handleWaitForCompanionApp();
  void handleLoadList();
  void handleReceiveList();
  void handleBrowsingList();
  void handleLoadPage();
  void handleReceivingPage();
  void handleDisplayPage();
  void handleError();

  class ServerCallbacks : public NimBLEServerCallbacks {
    friend class KavitaMangaReaderActivity;
    KavitaMangaReaderActivity* activity;

    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo);
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason);

   protected:
    explicit ServerCallbacks(KavitaMangaReaderActivity* activity) : activity(activity) {}
  };
  ServerCallbacks serverCallbacks;

  class RequestCallbacks : public NimBLECharacteristicCallbacks {
    friend class KavitaMangaReaderActivity;
    KavitaMangaReaderActivity* activity;

    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo);

   protected:
    explicit RequestCallbacks(KavitaMangaReaderActivity* activity) : activity(activity) {}
  };

  RequestCallbacks requestCallbacks;
  NimBLECharacteristic* pResponseChar = nullptr;
};
