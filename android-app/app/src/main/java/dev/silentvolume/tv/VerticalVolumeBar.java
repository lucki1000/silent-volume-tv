package dev.silentvolume.tv;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.util.AttributeSet;
import android.widget.ProgressBar;

/** A read-only pill slider, filled from the bottom using the stream's actual min/max. */
public final class VerticalVolumeBar extends ProgressBar {
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path clip = new Path();
    private final RectF track = new RectF();

    public VerticalVolumeBar(Context context, AttributeSet attributes) {
        super(context, attributes, android.R.attr.progressBarStyleHorizontal);
        setIndeterminate(false);
    }

    @Override
    protected synchronized void onDraw(Canvas canvas) {
        track.set(getPaddingLeft(), getPaddingTop(), getWidth() - getPaddingRight(),
                getHeight() - getPaddingBottom());
        float radius = track.width() / 2f;
        paint.setColor(Color.rgb(66, 69, 77));
        canvas.drawRoundRect(track, radius, radius, paint);

        int range = getMax() - getMin();
        float fraction = range > 0 ? (getProgress() - getMin()) / (float) range : 0f;
        fraction = Math.max(0f, Math.min(1f, fraction));
        if (fraction > 0f) {
            clip.reset();
            clip.addRoundRect(track, radius, radius, Path.Direction.CW);
            int saved = canvas.save();
            canvas.clipPath(clip);
            paint.setColor(Color.rgb(128, 203, 196));
            canvas.drawRect(track.left, track.bottom - track.height() * fraction,
                    track.right, track.bottom, paint);
            canvas.restoreToCount(saved);
        }
    }
}
