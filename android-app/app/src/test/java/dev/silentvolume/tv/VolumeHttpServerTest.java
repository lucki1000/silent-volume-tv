package dev.silentvolume.tv;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import org.junit.After;
import org.junit.Before;
import org.junit.Test;
import static org.junit.Assert.*;

public final class VolumeHttpServerTest {
    private static final String TOKEN = "0123456789abcdef0123456789abcdef";
    private final VolumeControllerTest.FakeAudio audio = new VolumeControllerTest.FakeAudio();
    private VolumeHttpServer server;

    @Before public void start() throws Exception {
        server = startServer(0);
    }

    private VolumeHttpServer startServer(int port) throws Exception {
        CountDownLatch ready = new CountDownLatch(1);
        VolumeHttpServer listener = new VolumeHttpServer(port,
                new VolumeApi(TOKEN, new VolumeController(audio)), ready::countDown);
        listener.start();
        assertTrue("Listener must bind", ready.await(3, TimeUnit.SECONDS));
        assertNull(listener.error);
        assertTrue(listener.listeningPort > 0);
        return listener;
    }

    @After public void stop() {
        if (server != null) server.close();
    }

    private Socket connect() throws IOException {
        Socket socket = new Socket("127.0.0.1", server.listeningPort);
        socket.setSoTimeout(6000);
        return socket;
    }

    private String request(String raw) throws IOException {
        try (Socket socket = connect()) {
            socket.getOutputStream().write(raw.getBytes(StandardCharsets.US_ASCII));
            socket.shutdownOutput();
            return read(socket);
        }
    }

    private static String read(Socket socket) throws IOException {
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        byte[] buffer = new byte[1024];
        int count;
        while ((count = socket.getInputStream().read(buffer)) != -1) output.write(buffer, 0, count);
        return new String(output.toByteArray(), StandardCharsets.UTF_8);
    }

    private String authenticated(String method, String path) throws IOException {
        return request(method + " " + path + " HTTP/1.1\r\nHost: tv\r\n"
                + "authorization: Bearer " + TOKEN + "\r\nContent-Length: 0\r\n\r\n");
    }

    private void code(int expected, String response) {
        assertTrue(response, response.startsWith("HTTP/1.1 " + expected + " "));
    }

    @Test public void receivesCommandsOverRealSocketsWithSilentFlags() throws Exception {
        String up = authenticated("POST", "/volume/up");
        code(200, up);
        assertTrue(up.contains("\"volume\":6"));
        String down = authenticated("POST", "/volume/down");
        code(200, down);
        assertTrue(down.contains("\"volume\":5"));
        assertEquals(0, audio.lastFlags);
        assertEquals(2, audio.calls);
    }

    @Test public void missingOrWrongTokenCannotChangeVolume() throws Exception {
        code(401, request("POST /volume/up HTTP/1.1\r\nHost: tv\r\n\r\n"));
        code(401, request("POST /volume/up HTTP/1.1\r\nAuthorization: Bearer wrong\r\n\r\n"));
        assertEquals(0, audio.calls);
    }

    @Test public void statusIsReadOnlyAndGetCannotIssueCommands() throws Exception {
        String status = authenticated("GET", "/status");
        code(200, status);
        assertTrue(status.contains("\"fixedVolume\":false"));
        code(405, authenticated("GET", "/volume/up"));
        code(405, authenticated("POST", "/status"));
        code(404, authenticated("POST", "/unknown"));
        assertEquals(0, audio.calls);
    }

    @Test public void fixedVolumeIsReportedAsUnsupported() throws Exception {
        audio.fixed = true;
        String response = authenticated("POST", "/volume/down");
        code(409, response);
        assertTrue(response.contains("fixed_volume"));
        assertTrue(authenticated("GET", "/status").contains("\"fixedVolume\":true"));
        assertEquals(0, audio.calls);
    }

    @Test public void integerEndpointReturnsOnlyTheCurrentNumberAsPlainText() throws Exception {
        audio.max = 30;
        audio.volume = 17;
        String response = authenticated("GET", "/volume/integer");
        code(200, response);
        assertTrue(response.contains("Content-Type: text/plain; charset=utf-8\r\n"));
        assertTrue(response.contains("Content-Length: 2\r\n"));
        assertEquals("17", response.split("\r\n\r\n", 2)[1]);
        assertEquals(0, audio.calls);

        code(200, authenticated("POST", "/volume/down"));
        assertEquals("16", authenticated("GET", "/volume/integer").split("\r\n\r\n", 2)[1]);
        audio.volume = 0;
        assertEquals("0", authenticated("GET", "/volume/integer").split("\r\n\r\n", 2)[1]);
        assertEquals(1, audio.calls);
    }

    @Test public void integerEndpointRequiresAuthenticationAndGet() throws Exception {
        String unauthorized = request("GET /volume/integer HTTP/1.1\r\nHost: tv\r\n\r\n");
        code(401, unauthorized);
        assertTrue(unauthorized.contains("Content-Type: application/json; charset=utf-8\r\n"));
        code(401, request("GET /volume/integer HTTP/1.1\r\nAuthorization: Bearer wrong\r\n\r\n"));
        code(405, authenticated("POST", "/volume/integer"));
        assertEquals(0, audio.calls);
    }

    @Test public void integerEndpointCanReadFixedVolumeDevices() throws Exception {
        audio.fixed = true;
        String response = authenticated("GET", "/volume/integer");
        code(200, response);
        assertEquals("5", response.split("\r\n\r\n", 2)[1]);
        assertEquals(0, audio.calls);
    }

    @Test public void rejectsAmbiguousHeadersAndRequestBodies() throws Exception {
        String base = "POST /volume/up HTTP/1.1\r\nAuthorization: Bearer " + TOKEN + "\r\n";
        code(400, request(base + "authorization: Bearer " + TOKEN + "\r\n\r\n"));
        code(400, request(base + "Content-Length: 1\r\n\r\nx"));
        code(400, request(base + "Transfer-Encoding: chunked\r\n\r\n"));
        assertEquals(0, audio.calls);
        code(200, authenticated("GET", "/status"));
    }

    @Test public void acceptsFragmentedNetworkPackets() throws Exception {
        try (Socket socket = connect()) {
            socket.getOutputStream().write("POST /volume/".getBytes(StandardCharsets.US_ASCII));
            socket.getOutputStream().flush();
            socket.getOutputStream().write(("up HTTP/1.1\r\nAuthorization: Bearer " + TOKEN
                    + "\r\n\r\n").getBytes(StandardCharsets.US_ASCII));
            socket.shutdownOutput();
            code(200, read(socket));
        }
    }

    @Test public void capsHeaderSizeBeforeExecutingACommand() throws Exception {
        String prefix = "POST /volume/up HTTP/1.1\r\nX-Long: ";
        char[] padding = new char[8193 - prefix.length()];
        Arrays.fill(padding, 'a');
        code(431, request(prefix + new String(padding)));
        assertEquals(0, audio.calls);
        code(200, authenticated("GET", "/status"));
    }

    @Test public void timesOutIncompleteRequestsAndKeepsListening() throws Exception {
        try (Socket socket = connect()) {
            socket.getOutputStream().write("POST /volume/up HTTP/1.1\r\n".getBytes(StandardCharsets.US_ASCII));
            socket.getOutputStream().flush();
            code(408, read(socket));
        }
        assertEquals(0, audio.calls);
        code(200, authenticated("GET", "/status"));
    }

    @Test public void stoppingReleasesThePortForReconfiguration() throws Exception {
        int port = server.listeningPort;
        server.close();
        assertFalse(server.isRunning());
        server = startServer(port);
        code(200, authenticated("GET", "/status"));
    }

    @Test public void immediateStopCannotBindAfterAReplacementStarts() throws Exception {
        int port = server.listeningPort;
        server.close();
        // Exercise shutdown while the listener thread may still be waiting to run.
        for (int i = 0; i < 10; i++) {
            VolumeHttpServer pending = new VolumeHttpServer(port,
                    new VolumeApi(TOKEN, new VolumeController(audio)), () -> { });
            pending.start();
            pending.close();
        }
        server = startServer(port);
        code(200, authenticated("GET", "/status"));
    }
}
