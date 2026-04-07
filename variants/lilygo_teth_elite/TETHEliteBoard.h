#pragma once

#include <helpers/ESP32Board.h>

namespace fs {
class FS;
}

using TETHEliteCommandHandler = void (*)(char* command, char* reply);

class TETHEliteBoard : public ESP32Board {
public:
  void begin();

  const char* getManufacturerName() const override {
    return "LilyGO T-ETH Elite";
  }

  bool startOTAUpdate(const char* id, char reply[]) override;

  void configureNetworkServices(fs::FS* fs, const char* node_name, const char* role,
                                const char* node_id, TETHEliteCommandHandler handler);
  void loopNetworkServices();
  void consolePrintLine(const char* line);
  void consolePrintf(const char* format, ...);
  bool ethernetReady() const;
};
