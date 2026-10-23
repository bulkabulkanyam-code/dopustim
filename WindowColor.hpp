// Frame Window Mod - one colour scale for the table and the replay markers.
#pragma once
#include <cocos2d.h>

namespace fwm {

// 1 frame = red, 2 = orange, 3 = yellow, 4-5 = light green, 6+ = green;
// capped ("N+", exact value unknown) = blue; unstable = grey.
inline cocos2d::ccColor3B windowColor(int w, bool capped = false, bool unstable = false) {
    if (unstable) return {170, 170, 170};
    if (capped) return {90, 200, 255};
    if (w <= 1) return {255, 80, 80};
    if (w == 2) return {255, 150, 60};
    if (w == 3) return {255, 225, 70};
    if (w <= 5) return {170, 235, 90};
    return {70, 225, 120};
}

}  // namespace fwm
