#pragma once

#include "CameraSettings.h"

bool isCaptureName(const String &name, const char *extension);
void configureCameraWeb(CameraSettings &settings, bool sdReady, const bool &captureActive,
                        void (*openWifiPortal)());
bool cameraWebRestartPending();
void beginCameraWeb();
void serviceCameraWeb(bool listening);
void stopCameraWeb();
