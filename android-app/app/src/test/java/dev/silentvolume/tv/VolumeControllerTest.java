package dev.silentvolume.tv;

import org.junit.Test;
import static org.junit.Assert.*;

public final class VolumeControllerTest {
    static final class FakeAudio implements VolumeController.AudioBackend {
        int volume = 5;
        int max = 10;
        int calls;
        int lastFlags = -1;
        boolean fixed;

        @Override public VolumeController.State read() {
            return new VolumeController.State(volume, 0, max, false, fixed);
        }
        @Override public void adjust(int direction, int flags) {
            calls++;
            lastFlags = flags;
            volume = Math.max(0, Math.min(max, volume + direction));
        }
    }

    @Test public void changesOneStepAndNeverRequestsAnOverlay() {
        FakeAudio audio = new FakeAudio();
        VolumeController controller = new VolumeController(audio);
        assertEquals(6, controller.adjust(1).volume);
        assertEquals(5, controller.adjust(-1).volume);
        assertEquals(2, audio.calls);
        assertEquals(0, audio.lastFlags);
    }

    @Test public void fixedVolumeFailsWithoutMakingAnAudioCall() {
        FakeAudio audio = new FakeAudio();
        audio.fixed = true;
        try {
            new VolumeController(audio).adjust(-1);
            fail("Fixed-volume devices must not silently report success");
        } catch (UnsupportedOperationException expected) {
            assertEquals(0, audio.calls);
        }
    }

    @Test public void reportsActualVolumeAtStreamBoundaries() {
        FakeAudio audio = new FakeAudio();
        VolumeController controller = new VolumeController(audio);
        audio.volume = 10;
        assertEquals(10, controller.adjust(1).volume);
        audio.volume = 0;
        assertEquals(0, controller.adjust(-1).volume);
    }
}
