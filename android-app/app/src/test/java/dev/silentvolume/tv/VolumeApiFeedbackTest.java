package dev.silentvolume.tv;

import java.util.concurrent.atomic.AtomicInteger;
import org.junit.Test;
import static org.junit.Assert.*;

public final class VolumeApiFeedbackTest {
    private static final String TOKEN = "0123456789abcdef0123456789abcdef";
    private static final String AUTH = "Bearer " + TOKEN;

    @Test public void onlySuccessfulVolumeCommandsRequestFeedback() {
        AtomicInteger calls = new AtomicInteger();
        VolumeControllerTest.FakeAudio audio = new VolumeControllerTest.FakeAudio();
        VolumeApi api = new VolumeApi(TOKEN, new VolumeController(audio), calls::incrementAndGet);
        assertEquals(200, api.handle("GET", "/status", AUTH).code);
        assertEquals(200, api.handle("GET", "/volume/integer", AUTH).code);
        assertEquals(401, api.handle("POST", "/volume/up", null).code);
        assertEquals(405, api.handle("GET", "/volume/up", AUTH).code);
        assertEquals(0, calls.get());
        assertEquals(200, api.handle("POST", "/volume/up", AUTH).code);
        assertEquals(200, api.handle("POST", "/volume/down", AUTH).code);
        assertEquals(2, calls.get());
        audio.fixed = true;
        assertEquals(409, api.handle("POST", "/volume/down", AUTH).code);
        assertEquals(2, calls.get());
    }

    @Test public void feedbackFailureCannotFailASuccessfulVolumeCommand() {
        VolumeControllerTest.FakeAudio audio = new VolumeControllerTest.FakeAudio();
        VolumeApi api = new VolumeApi(TOKEN, new VolumeController(audio), () -> {
            throw new SecurityException("Overlay permission denied");
        });
        VolumeApi.Response response = api.handle("POST", "/volume/up", AUTH);
        assertEquals(200, response.code);
        assertEquals(6, audio.volume);
        assertTrue(response.body.contains("\"volume\":6"));
        assertEquals(0, audio.lastFlags);
    }
}
