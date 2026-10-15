package dev.silentvolume.tv;

/** Serializes commands from network workers and the local test buttons. */
final class VolumeController {
    interface AudioBackend {
        State read();
        void adjust(int direction, int flags);
    }

    static final class State {
        final int volume;
        final int min;
        final int max;
        final boolean muted;
        final boolean fixed;

        State(int volume, int min, int max, boolean muted, boolean fixed) {
            this.volume = volume;
            this.min = min;
            this.max = max;
            this.muted = muted;
            this.fixed = fixed;
        }

        String json() {
            return "{\"volume\":" + volume + ",\"min\":" + min + ",\"max\":" + max
                    + ",\"muted\":" + muted + ",\"fixedVolume\":" + fixed + "}";
        }
    }

    private final AudioBackend backend;

    VolumeController(AudioBackend backend) {
        this.backend = backend;
    }

    synchronized State status() {
        return backend.read();
    }

    synchronized State adjust(int direction) {
        if (direction != 1 && direction != -1) {
            throw new IllegalArgumentException("Only volume up/down is supported");
        }
        if (backend.read().fixed) {
            throw new UnsupportedOperationException("fixed_volume");
        }
        // Zero deliberately excludes FLAG_SHOW_UI, FLAG_PLAY_SOUND and FLAG_VIBRATE.
        backend.adjust(direction, 0);
        return backend.read();
    }
}
