package dev.silentvolume.tv;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.InetSocketAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.net.SocketTimeoutException;
import java.nio.charset.StandardCharsets;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;

/** Small, bounded HTTP/1.x receiver. Each request uses a separate, short-lived connection. */
final class VolumeHttpServer implements AutoCloseable {
    private static final int MAX_HEADER_BYTES = 8192;
    private static final int REQUEST_TIMEOUT_MS = 3000;
    private final int requestedPort;
    private final VolumeApi api;
    private final Runnable stateChanged;
    private final Set<Socket> clients = ConcurrentHashMap.newKeySet();
    private final ThreadPoolExecutor workers = new ThreadPoolExecutor(2, 2, 0, TimeUnit.MILLISECONDS,
            new ArrayBlockingQueue<>(8), runnable -> {
                Thread thread = new Thread(runnable, "volume-request");
                thread.setDaemon(true);
                return thread;
            });
    private volatile boolean running;
    private volatile ServerSocket listener;
    volatile int listeningPort = -1;
    volatile String error;

    VolumeHttpServer(int port, VolumeApi api, Runnable stateChanged) {
        requestedPort = port;
        this.api = api;
        this.stateChanged = stateChanged;
    }

    synchronized void start() {
        if (running || workers.isShutdown()) {
            throw new IllegalStateException("Server instances can only be started once");
        }
        running = true;
        Thread thread = new Thread(this::listen, "volume-listener");
        thread.setDaemon(true);
        thread.start();
    }

    boolean isRunning() { return running; }

    private void listen() {
        try (ServerSocket socket = new ServerSocket()) {
            synchronized (this) {
                // A close before the listener thread starts must prevent a later bind.
                if (!running) return;
                listener = socket;
                socket.setReuseAddress(true);
                socket.bind(new InetSocketAddress(requestedPort), 8);
                listeningPort = socket.getLocalPort();
            }
            stateChanged.run();
            while (running) {
                Socket client = socket.accept();
                clients.add(client);
                if (!running) {
                    closeClient(client);
                    break;
                }
                try {
                    workers.execute(() -> serve(client));
                } catch (RejectedExecutionException busy) {
                    try {
                        write(client, VolumeApi.Response.error(503, "receiver_busy"));
                    } catch (IOException ignored) {
                        // A rejected client disconnecting must not stop the accept loop.
                    } finally {
                        closeClient(client);
                    }
                }
            }
        } catch (IOException e) {
            if (running) {
                error = "Cannot listen on port " + requestedPort + " (" + e.getClass().getSimpleName() + ")";
            }
        } finally {
            close();
            stateChanged.run();
        }
    }

    private void serve(Socket socket) {
        try {
            socket.setSoTimeout(REQUEST_TIMEOUT_MS);
            VolumeApi.Response response;
            try {
                response = parse(socket.getInputStream());
            } catch (SocketTimeoutException e) {
                response = VolumeApi.Response.error(408, "request_timeout");
            } catch (BadRequest e) {
                response = VolumeApi.Response.error(e.code, e.getMessage());
            }
            write(socket, response);
        } catch (IOException ignored) {
            // A disconnected controller must not stop the listener.
        } finally {
            closeClient(socket);
        }
    }

    private VolumeApi.Response parse(InputStream input) throws IOException, BadRequest {
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        long deadline = System.nanoTime() + TimeUnit.MILLISECONDS.toNanos(REQUEST_TIMEOUT_MS);
        int end = 0;
        while (end != 0x0d0a0d0a) {
            if (System.nanoTime() >= deadline) throw new SocketTimeoutException();
            int next = input.read();
            if (next < 0) throw new BadRequest(400, "incomplete_request");
            if (next > 127 || next == 0) throw new BadRequest(400, "invalid_header");
            bytes.write(next);
            if (bytes.size() > MAX_HEADER_BYTES) throw new BadRequest(431, "headers_too_large");
            end = (end << 8) | next;
        }
        String[] lines = new String(bytes.toByteArray(), StandardCharsets.US_ASCII).split("\r\n");
        String[] request = lines[0].split(" ", -1);
        if (request.length != 3 || !("HTTP/1.1".equals(request[2]) || "HTTP/1.0".equals(request[2]))) {
            throw new BadRequest(400, "invalid_request_line");
        }
        Map<String, String> headers = new HashMap<>();
        for (int i = 1; i < lines.length; i++) {
            int colon = lines[i].indexOf(':');
            if (colon < 1) throw new BadRequest(400, "invalid_header");
            String name = lines[i].substring(0, colon).toLowerCase(Locale.ROOT);
            if (!name.matches("[a-z0-9!#$%&'*+.^_`|~-]+")) throw new BadRequest(400, "invalid_header");
            if (headers.put(name, lines[i].substring(colon + 1).trim()) != null) {
                throw new BadRequest(400, "duplicate_header");
            }
        }
        // No request bodies are needed; reject ambiguous framing and never read unbounded data.
        if (headers.containsKey("transfer-encoding")) throw new BadRequest(400, "body_not_supported");
        if (headers.containsKey("content-length") && !"0".equals(headers.get("content-length"))) {
            throw new BadRequest(400, "body_not_supported");
        }
        if (!running) return VolumeApi.Response.error(503, "receiver_stopping");
        return api.handle(request[0], request[1], headers.get("authorization"));
    }

    private static void write(Socket socket, VolumeApi.Response response) throws IOException {
        byte[] body = response.body.getBytes(StandardCharsets.UTF_8);
        String headers = "HTTP/1.1 " + response.code + " " + reason(response.code) + "\r\n"
                + "Content-Type: " + response.contentType + "\r\n"
                + "Content-Length: " + body.length + "\r\n"
                + "Cache-Control: no-store\r\nConnection: close\r\n"
                + (response.code == 401 ? "WWW-Authenticate: Bearer\r\n" : "")
                + "\r\n";
        socket.getOutputStream().write(headers.getBytes(StandardCharsets.US_ASCII));
        socket.getOutputStream().write(body);
        socket.getOutputStream().flush();
    }

    private static String reason(int code) {
        switch (code) {
            case 200: return "OK";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Method Not Allowed";
            case 408: return "Request Timeout";
            case 409: return "Conflict";
            case 431: return "Request Header Fields Too Large";
            default: return "Service Unavailable";
        }
    }

    private void closeClient(Socket socket) {
        clients.remove(socket);
        try { socket.close(); } catch (IOException ignored) { }
    }

    @Override
    public synchronized void close() {
        running = false;
        listeningPort = -1;
        ServerSocket socket = listener;
        if (socket != null) {
            try { socket.close(); } catch (IOException ignored) { }
        }
        for (Socket client : clients) closeClient(client);
        workers.shutdownNow();
    }

    private static final class BadRequest extends Exception {
        final int code;
        BadRequest(int code, String message) {
            super(message);
            this.code = code;
        }
    }
}
