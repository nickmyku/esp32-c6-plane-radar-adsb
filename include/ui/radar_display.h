#pragma once

namespace ui {

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Redraw aircraft only (blits cached grid; no full-screen clear). */
void radarDisplayRefreshAircraft();

/**
 * Free the off-screen frame so the ADS-B task can allocate TLS buffers.
 * Blocks until the UI task has released the sprite (or the wait times out).
 * Call only from the fetch task, never from the loop task.
 */
void radarDisplayPauseForFetch();

/** Allow the UI task to allocate the frame and draw again. */
void radarDisplayResumeAfterFetch();

}  // namespace ui
