#pragma once

/** Bring up the onboard CST816S. Safe to call if the controller is absent. */
void touchInit();
/** Latched screen tap. Call from the main loop; a tap cycles the radar range. */
bool touchConsumeTap();
