package com.limelight.binding.video;

/** Per-stream, command-driven collection. No timer or background worker. */
public final class RemoteStatsSession {
    public static final int MAX_SECONDS = 60;
    public static final int WARMUP_MS = 3000;

    public static final class Session {
        public final String id;
        public final long startedElapsedMs, startedUptimeMs, expiresElapsedMs;
        // Written and consumed only by the video input thread.
        long handoffSumNs, handoffCount;
        Session(String id, long elapsedMs, long uptimeMs, int seconds) {
            this.id = id;
            startedElapsedMs = elapsedMs;
            startedUptimeMs = uptimeMs;
            expiresElapsedMs = elapsedMs + seconds * 1000L;
        }
    }

    private volatile Session session;
    private boolean closed;

    public Session peek() { return session; }

    public Session current(long elapsedMs) {
        Session value = session;
        if (value != null && elapsedMs >= value.expiresElapsedMs) {
            synchronized (this) {
                if (session == value) session = null;
                return session;
            }
        }
        return value;
    }

    public synchronized Session start(String id, int seconds, long elapsedMs, long uptimeMs) {
        if (closed) throw new IllegalStateException("no_stream");
        if (seconds < 1 || seconds > MAX_SECONDS || elapsedMs > Long.MAX_VALUE - seconds * 1000L)
            throw new IllegalArgumentException("seconds must be 1..60");
        session = new Session(id, elapsedMs, uptimeMs, seconds);
        return session;
    }

    public synchronized Session stop() {
        Session previous = session;
        session = null;
        return previous;
    }

    public synchronized void close() {
        closed = true;
        session = null;
    }
}
