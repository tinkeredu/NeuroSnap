#pragma once

#include "CameraSettings.h"

bool isCaptureName(const String &name, const char *extension);
void configureCameraWeb(CameraSettings &settings, bool sdReady, void (*openWifiPortal)());
void beginCameraWeb();
void serviceCameraWeb();
void stopCameraWeb();
