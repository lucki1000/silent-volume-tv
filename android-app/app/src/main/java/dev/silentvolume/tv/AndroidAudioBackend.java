package dev.silentvolume.tv;

import android.content.Context;
import android.media.AudioManager;

final class AndroidAudioBackend implements VolumeController.AudioBackend {
    private final AudioManager audio;
    private static VolumeController controller;

    private AndroidAudioBackend(Context context) {
        audio = context.getSystemService(AudioManager.class);
    }

    static synchronized VolumeController controller(Context context) {
        if (controller == null) {
            controller = new VolumeController(new AndroidAudioBackend(context.getApplicationContext()));
        }
        return controller;
    }

    @Override
    public VolumeController.State read() {
        return new VolumeController.State(
                audio.getStreamVolume(AudioManager.STREAM_MUSIC),
                audio.getStreamMinVolume(AudioManager.STREAM_MUSIC),
                audio.getStreamMaxVolume(AudioManager.STREAM_MUSIC),
                audio.isStreamMute(AudioManager.STREAM_MUSIC),
                audio.isVolumeFixed());
    }

    @Override
    public void adjust(int direction, int flags) {
        audio.adjustStreamVolume(AudioManager.STREAM_MUSIC, direction, flags);
    }
}
