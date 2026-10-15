package dev.silentvolume.tv;

import android.app.Application;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.view.WindowManager;
import android.view.Gravity;
import android.widget.ProgressBar;
import android.widget.TextView;
import java.time.Duration;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.LooperMode;
import org.robolectric.shadow.api.Shadow;
import org.robolectric.shadows.ShadowSettings;
import org.robolectric.shadows.ShadowWindowManagerImpl;
import static org.junit.Assert.*;
import static org.robolectric.Shadows.shadowOf;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 30)
@LooperMode(LooperMode.Mode.PAUSED)
public final class VolumeOverlayTest {
    private VolumeOverlay overlay;
    private ShadowWindowManagerImpl windows;

    @Before public void setup() {
        Application app = RuntimeEnvironment.getApplication();
        windows = Shadow.extract(app.getSystemService(WindowManager.class));
        overlay = new VolumeOverlay(app, new Handler(Looper.getMainLooper()));
        ShadowSettings.setCanDrawOverlays(true);
    }

    @After public void cleanup() { overlay.close(); }

    private VolumeController.State state(int volume) {
        return new VolumeController.State(volume, 0, 30, false, false);
    }

    @Test public void permissionIsRequiredAndCanBeGrantedLater() {
        ShadowSettings.setCanDrawOverlays(false);
        overlay.show(state(17));
        assertTrue(windows.getViews().isEmpty());
        ShadowSettings.setCanDrawOverlays(true);
        overlay.show(state(17));
        assertEquals(1, windows.getViews().size());
    }

    @Test public void showsActualVolumeAndDoesNotTakeFocusOrTouchInput() {
        overlay.show(state(17));
        View panel = windows.getViews().get(0);
        assertEquals("17", ((TextView) panel.findViewById(R.id.overlay_value)).getText().toString());
        ProgressBar progress = panel.findViewById(R.id.overlay_progress);
        assertEquals(17, progress.getProgress());
        assertEquals(30, progress.getMax());
        WindowManager.LayoutParams params = (WindowManager.LayoutParams) panel.getLayoutParams();
        assertEquals(WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY, params.type);
        assertTrue((params.flags & WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE) != 0);
        assertTrue((params.flags & WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE) != 0);
    }

    @Test public void expiresTwoSecondsAfterTheLatestCommandWithoutStackingWindows() {
        overlay.show(state(17));
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(1500));
        overlay.show(state(18));
        assertEquals(1, windows.getViews().size());
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(600));
        assertEquals(1, windows.getViews().size());
        View panel = windows.getViews().get(0);
        assertEquals("18", ((TextView) panel.findViewById(R.id.overlay_value)).getText().toString());
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(1400));
        assertTrue(windows.getViews().isEmpty());
    }

    @Test public void revokedPermissionOrServiceStopRemovesTheWindow() {
        overlay.show(state(17));
        ShadowSettings.setCanDrawOverlays(false);
        overlay.show(state(18));
        assertTrue(windows.getViews().isEmpty());
        ShadowSettings.setCanDrawOverlays(true);
        overlay.show(state(19));
        overlay.close();
        assertTrue(windows.getViews().isEmpty());
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(VolumeOverlay.DISPLAY_MILLIS));
        assertTrue(windows.getViews().isEmpty());
    }

    @Test public void defaultsToRightAndCanMoveAnExistingPanelToTheLeft() {
        overlay.show(state(17));
        View panel = windows.getViews().get(0);
        WindowManager.LayoutParams right = (WindowManager.LayoutParams) panel.getLayoutParams();
        assertEquals(Gravity.CENTER_VERTICAL | Gravity.RIGHT, right.gravity);
        assertEquals(0, right.y);
        overlay.setOnLeft(true);
        assertEquals(1, windows.getViews().size());
        assertSame(panel, windows.getViews().get(0));
        WindowManager.LayoutParams left = (WindowManager.LayoutParams) panel.getLayoutParams();
        assertEquals(Gravity.CENTER_VERTICAL | Gravity.LEFT, left.gravity);
        assertEquals(0, left.y);
    }

    @Test @Config(qualifiers = "mdpi")
    public void scalesTheEntirePanelAtStandardDensity() { assertScaling(); }

    @Test @Config(qualifiers = "xhdpi")
    public void scalesTheEntirePanelAtDoubleDensity() { assertScaling(); }

    @Test @Config(qualifiers = "xxxhdpi")
    public void scalesTheEntirePanelAtQuadrupleDensity() { assertScaling(); }

    private void assertScaling() {
        overlay.setOnLeft(true);
        overlay.show(state(17));
        float density = RuntimeEnvironment.getApplication().getResources().getDisplayMetrics().density;
        for (int percent : new int[] {100, 75, 125, 150}) {
            overlay.setScalePercent(percent);
            assertEquals("Resizing must keep a single window", 1, windows.getViews().size());
            View panel = windows.getViews().get(0);
            WindowManager.LayoutParams params = (WindowManager.LayoutParams) panel.getLayoutParams();
            assertEquals(Math.round(72 * density * percent / 100f), params.width);
            assertEquals("Size must not increase the edge gap", Math.round(8 * density), params.x);
            assertEquals(Gravity.CENTER_VERTICAL | Gravity.LEFT, params.gravity);
            panel.measure(View.MeasureSpec.makeMeasureSpec(params.width, View.MeasureSpec.EXACTLY),
                    View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
            panel.layout(0, 0, panel.getMeasuredWidth(), panel.getMeasuredHeight());
            ProgressBar bar = panel.findViewById(R.id.overlay_progress);
            TextView number = panel.findViewById(R.id.overlay_value);
            View speaker = panel.findViewById(R.id.overlay_speaker);
            assertEquals(Math.round(168 * density * percent / 100f), bar.getHeight());
            assertEquals(Math.round(28 * density * percent / 100f), bar.getWidth());
            assertEquals(Math.round(20 * density * percent / 100f), speaker.getWidth());
            assertEquals(24 * density * percent / 100f, number.getTextSize(), 1f);
            assertEquals("17", number.getText().toString());
            assertTrue(number.getBottom() < bar.getTop());
            assertTrue("Whole speaker must fit the window", speaker.getBottom() <= panel.getHeight());
        }
        shadowOf(Looper.getMainLooper()).idleFor(Duration.ofMillis(VolumeOverlay.DISPLAY_MILLIS));
        assertTrue(windows.getViews().isEmpty());
    }
}
