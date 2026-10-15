package dev.silentvolume.tv;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.view.View;
import android.widget.TextView;
import java.io.File;
import java.io.FileOutputStream;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.robolectric.RobolectricTestRunner;
import org.robolectric.RuntimeEnvironment;
import org.robolectric.annotation.Config;
import org.robolectric.annotation.GraphicsMode;
import static org.junit.Assert.*;

@RunWith(RobolectricTestRunner.class)
@Config(sdk = 30, qualifiers = "xhdpi")
@GraphicsMode(GraphicsMode.Mode.NATIVE)
public final class VolumeOverlayRenderTest {
    @Test public void rendersBottomUpFillWithTheNumberAboveIt() throws Exception {
        Context context = RuntimeEnvironment.getApplication();
        View panel = VolumeOverlay.inflatePanel(context, 100);
        TextView number = panel.findViewById(R.id.overlay_value);
        number.setText("17");
        VerticalVolumeBar bar = panel.findViewById(R.id.overlay_progress);
        bar.setMin(0);
        bar.setMax(30);
        bar.setProgress(17);
        int width = panel.getContext().getResources().getDimensionPixelSize(R.dimen.overlay_width);
        panel.measure(View.MeasureSpec.makeMeasureSpec(width, View.MeasureSpec.EXACTLY),
                View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        panel.layout(0, 0, panel.getMeasuredWidth(), panel.getMeasuredHeight());
        assertTrue("Current value must sit above the bar", number.getBottom() < bar.getTop());
        assertTrue("Volume bar must be vertical", bar.getHeight() > bar.getWidth());

        Bitmap partial = render(bar);
        int center = partial.getWidth() / 2;
        int top = partial.getHeight() / 4;
        int bottom = partial.getHeight() * 3 / 4;
        int emptyColor = partial.getPixel(center, top);
        int filledColor = partial.getPixel(center, bottom);
        assertNotEquals("Partial volume must fill the lower part", emptyColor, filledColor);

        bar.setProgress(0);
        Bitmap empty = render(bar);
        assertEquals(emptyColor, empty.getPixel(center, bottom));
        bar.setProgress(30);
        Bitmap full = render(bar);
        assertEquals(filledColor, full.getPixel(center, top));
        bar.setMin(10);
        bar.setProgress(10);
        assertEquals(emptyColor, render(bar).getPixel(center, bottom));
        bar.setMin(0);
        bar.setProgress(17);

        File directory = new File(System.getProperty("silentvolume.preview.dir"));
        assertTrue(directory.isDirectory() || directory.mkdirs());
        try (FileOutputStream output = new FileOutputStream(new File(directory, "volume-overlay.png"))) {
            assertTrue(render(panel).compress(Bitmap.CompressFormat.PNG, 100, output));
        }
    }

    private static Bitmap render(View view) {
        Bitmap bitmap = Bitmap.createBitmap(view.getWidth(), view.getHeight(), Bitmap.Config.ARGB_8888);
        view.draw(new Canvas(bitmap));
        return bitmap;
    }
}
