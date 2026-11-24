#include <game/client/components/quick_zoom.h>

#include <gtest/gtest.h>

TEST(CameraQuickZoom, TurningOnSelectsQuickTargetAndTurningOffSelectsConfiguredDefault)
{
	constexpr float DefaultZoom = 0.73f;
	constexpr float QuickZoom = 1.24f;
	EXPECT_FLOAT_EQ(tclient_quick_zoom::SelectTarget(false, DefaultZoom, QuickZoom), QuickZoom);
	EXPECT_FLOAT_EQ(tclient_quick_zoom::SelectTarget(true, DefaultZoom, QuickZoom), DefaultZoom);
}
