#pragma once

#include <helpers/ESP32Board.h>

class FoxNanoS3Board : public ESP32Board {
public:
  const char* getManufacturerName() const override {
    return "fox-nano-s3";
  }
};
