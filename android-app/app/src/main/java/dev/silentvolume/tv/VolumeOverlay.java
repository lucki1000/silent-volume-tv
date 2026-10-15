package dev.silentvolume.tv;

import android.annotation.SuppressLint;
import android.content.Context;
import android.content.res.Configuration;
import android.graphics.PixelFormat;
import android.os.Handler;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.LayoutInflater;
import android.view.View;
import android.view.WindowManager;
import android.widget.ProgressBar;
import android.widget.TextView;
import android.widget.ImageView;

/** All window operations run on the service's main thread. */
final class VolumeOverlay implements AutoCloseable {
    static final long DISPLAY_MILLIS = 2000;
    private final Context context;
    private final Handler handler;
    private final WindowManager windows;
    private final Runnable dismiss = this::hide;
    private View view;
    private boolean onLeft;
    private int scalePercent = 100;
    private VolumeController.State lastState;

    VolumeOverlay(Context context, Handler handler) {
        this.context = context;
        this.handler = handler;
        windows = context.getSystemService(WindowManager.class);
    }

    void show(VolumeController.State state) {
        if (!Settings.canDrawOverlays(context)) {
            hide();
            return;
        }
        try {
            if (view == null) {
                View panel = inflatePanel(context, scalePercent);
                WindowManager.LayoutParams params = new WindowManager.LayoutParams(
                        panel.getContext().getResources().getDimensionPixelSize(R.dimen.overlay_width),
                        WindowManager.LayoutParams.WRAP_CONTENT,
                        WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
                        WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE | WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE,
                        PixelFormat.TRANSLUCENT);
                position(params);
                params.setTitle("Silent Volume TV volume");
                windows.addView(panel, params);
                view = panel;
            }
            TextView value = view.findViewById(R.id.overlay_value);
            value.setText(context.getString(R.string.overlay_value, state.volume));
            ImageView speaker = view.findViewById(R.id.overlay_speaker);
            speaker.setImageResource(state.muted || state.volume == state.min
                    ? R.drawable.ic_speaker_muted : R.drawable.ic_speaker);
            ProgressBar bar = view.findViewById(R.id.overlay_progress);
            bar.setMin(state.min);
            bar.setMax(state.max);
            bar.setProgress(state.volume);
            lastState = state;
            handler.removeCallbacks(dismiss);
            handler.postDelayed(dismiss, DISPLAY_MILLIS);
        } catch (RuntimeException e) {
            Log.w("SilentVolumeTV", "Cannot display volume overlay", e);
            hide();
        }
    }

    static View inflatePanel(Context context, int scalePercent) {
        // Scale layout, text and vector drawables together without clipping a transformed view.
        Configuration configuration = new Configuration(context.getResources().getConfiguration());
        configuration.densityDpi = Math.round(
                context.getResources().getDisplayMetrics().densityDpi * scalePercent / 100f);
        Context scaledContext = context.createConfigurationContext(configuration);
        return LayoutInflater.from(scaledContext).inflate(R.layout.volume_overlay, null);
    }

    void setScalePercent(int value) {
        if (value < 75 || value > 150) throw new IllegalArgumentException("Overlay size must be 75–150%");
        if (scalePercent == value) return;
        scalePercent = value;
        if (view != null) {
            VolumeController.State state = lastState;
            hide();
            show(state);
        }
    }

    void setOnLeft(boolean value) {
        onLeft = value;
        if (view != null) {
            try {
                WindowManager.LayoutParams params = (WindowManager.LayoutParams) view.getLayoutParams();
                position(params);
                windows.updateViewLayout(view, params);
            } catch (RuntimeException e) {
                Log.w("SilentVolumeTV", "Cannot reposition volume overlay", e);
                hide();
            }
        }
    }

    @SuppressLint("RtlHardcoded")
    private void position(WindowManager.LayoutParams params) {
        // These are physical screen sides, independent of the language's reading direction.
        params.gravity = Gravity.CENTER_VERTICAL | (onLeft ? Gravity.LEFT : Gravity.RIGHT);
        params.x = dp(8);
        params.y = 0;
    }

    void hide() {
        handler.removeCallbacks(dismiss);
        View panel = view;
        view = null;
        if (panel != null) {
            try {
                windows.removeViewImmediate(panel);
            } catch (RuntimeException e) {
                Log.w("SilentVolumeTV", "Cannot remove volume overlay", e);
            }
        }
    }

    private int dp(int value) {
        return Math.round(value * context.getResources().getDisplayMetrics().density);
    }

    @Override
    public void close() { hide(); }
}
