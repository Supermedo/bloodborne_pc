// bbport: mousecam sampling on its own thread. The camera reads SDL relative
// motion at 1 kHz here instead of from the window event pump, so queued SDL
// events and pump timing stay out of the input-to-guest path.
#pragma once

namespace BbMouse {
// Starts the sampler; safe to call once per window lifetime.
void Start();
// Stops the sampler and joins the thread; the window pump must not run after.
void Stop();
// Gate snapshot published by the window thread once per pump.
void SetActive(bool active);
// Window thread, once per pump: a controller is in use, so the mouse moves only through a mapping
// program turning its stick into mouse movement; that motion is dropped, not turned into the camera.
void SetPadInUse(bool in_use);
} // namespace BbMouse
