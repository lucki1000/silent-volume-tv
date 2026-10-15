package dev.silentvolume.tv;

import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;

final class VolumeApi {
    static final class Response {
        final int code;
        final String body;
        final String contentType;

        Response(int code, String body) {
            this(code, body, "application/json; charset=utf-8");
        }

        Response(int code, String body, String contentType) {
            this.code = code;
            this.body = body;
            this.contentType = contentType;
        }

        static Response error(int code, String error) {
            return new Response(code, "{\"error\":\"" + error + "\"}");
        }
    }

    private final byte[] authorization;
    private final VolumeController controller;
    private final Runnable onVolumeCommand;

    VolumeApi(String token, VolumeController controller) {
        this(token, controller, () -> { });
    }

    VolumeApi(String token, VolumeController controller, Runnable onVolumeCommand) {
        if (token == null || token.length() < 32) {
            throw new IllegalArgumentException("A secure access token is required");
        }
        authorization = ("Bearer " + token).getBytes(StandardCharsets.US_ASCII);
        this.controller = controller;
        this.onVolumeCommand = onVolumeCommand;
    }

    Response handle(String method, String path, String auth) {
        if (auth == null || !MessageDigest.isEqual(authorization, auth.getBytes(StandardCharsets.US_ASCII))) {
            return Response.error(401, "unauthorized");
        }
        boolean status = "/status".equals(path);
        boolean integer = "/volume/integer".equals(path);
        boolean up = "/volume/up".equals(path);
        boolean down = "/volume/down".equals(path);
        if (!status && !integer && !up && !down) {
            return Response.error(404, "not_found");
        }
        if (!(status || integer ? "GET" : "POST").equals(method)) {
            return Response.error(405, "method_not_allowed");
        }
        try {
            if (integer) {
                return new Response(200, Integer.toString(controller.status().volume), "text/plain; charset=utf-8");
            }
            VolumeController.State state = status ? controller.status() : controller.adjust(up ? 1 : -1);
            if (!status) {
                try {
                    onVolumeCommand.run();
                } catch (RuntimeException ignored) {
                    // Optional visual feedback must not fail a command that already changed volume.
                }
            }
            return new Response(200, state.json());
        } catch (UnsupportedOperationException e) {
            return Response.error(409, "fixed_volume");
        } catch (SecurityException e) {
            return Response.error(403, "audio_permission_denied");
        } catch (RuntimeException e) {
            return Response.error(503, "audio_unavailable");
        }
    }
}
