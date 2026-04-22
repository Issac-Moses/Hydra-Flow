-- ============================================================
-- Hydra-Flow & AC Control - MASTER SETUP (v8.0)
-- Consolidated SQL for Water Motor and AC Control Systems
-- ============================================================

-- ── 1. TABLES: MOTOR SYSTEM ──────────────────────────────────

CREATE TABLE IF NOT EXISTS public.motor_system (
    id               INTEGER PRIMARY KEY CHECK (id = 1),
    motor_status     BOOLEAN NOT NULL DEFAULT FALSE,
    water_level      INTEGER NOT NULL DEFAULT 0,        
    mode             TEXT NOT NULL DEFAULT 'OFFLINE',   
    manual_override  BOOLEAN NOT NULL DEFAULT FALSE,
    auto_off_timeout INTEGER NOT NULL DEFAULT 30,       
    motor_on_time    INTEGER NOT NULL DEFAULT 0,        
    tank_last_seen   TIMESTAMPTZ DEFAULT '2000-01-01',
    motor_last_seen  TIMESTAMPTZ DEFAULT '2000-01-01',
    tank_ping        BOOLEAN NOT NULL DEFAULT FALSE,
    motor_ping       BOOLEAN NOT NULL DEFAULT FALSE,
    updated_at       TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS public.motor_schedules (
    id          BIGSERIAL PRIMARY KEY,
    on_time     TEXT NOT NULL,                      
    enabled     BOOLEAN NOT NULL DEFAULT TRUE,
    created_at  TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS public.motor_logs (
    id          BIGSERIAL PRIMARY KEY,
    status      BOOLEAN NOT NULL,
    water_level INTEGER NOT NULL DEFAULT 0,
    mode        TEXT NOT NULL DEFAULT 'ONLINE',
    reason      TEXT DEFAULT 'manual',  
    timestamp   TIMESTAMPTZ DEFAULT now()
);

-- ── 2. TABLES: AC SYSTEM ─────────────────────────────────────

CREATE TABLE IF NOT EXISTS public.ac_system (
    id              INTEGER PRIMARY KEY CHECK (id = 1),
    ac_status       BOOLEAN NOT NULL DEFAULT FALSE,
    mode            TEXT NOT NULL DEFAULT 'OFFLINE',
    ac_last_seen    TIMESTAMPTZ DEFAULT '2000-01-01',
    ac_ping         BOOLEAN NOT NULL DEFAULT FALSE,
    updated_at      TIMESTAMPTZ DEFAULT NOW()
);

CREATE TABLE IF NOT EXISTS public.ac_schedules (
    id          BIGSERIAL PRIMARY KEY,
    on_time     TEXT NOT NULL,
    off_time    TEXT NOT NULL,
    enabled     BOOLEAN NOT NULL DEFAULT TRUE,
    created_at  TIMESTAMPTZ DEFAULT now()
);

CREATE TABLE IF NOT EXISTS public.ac_logs (
    id          BIGSERIAL PRIMARY KEY,
    status      BOOLEAN NOT NULL,
    mode        TEXT NOT NULL DEFAULT 'ONLINE',
    reason      TEXT DEFAULT 'manual',
    timestamp   TIMESTAMPTZ DEFAULT now()
);

-- ── 3. INITIAL SEED ──────────────────────────────────────────

INSERT INTO public.motor_system (id, motor_status, water_level, mode)
VALUES (1, FALSE, 0, 'OFFLINE')
ON CONFLICT (id) DO NOTHING;

INSERT INTO public.ac_system (id, ac_status, mode)
VALUES (1, FALSE, 'OFFLINE')
ON CONFLICT (id) DO NOTHING;

-- ── 4. AUTOMATION LOGIC: MOTOR ────────────────────────────────

CREATE OR REPLACE FUNCTION public.handle_motor_automation()
RETURNS TRIGGER AS $$
DECLARE
    motor_rejected BOOLEAN := FALSE;
    log_reason     TEXT    := 'system';
BEGIN
    -- Heartbeat Detection
    IF (COALESCE(OLD.tank_ping, FALSE) IS DISTINCT FROM NEW.tank_ping) THEN
        NEW.tank_last_seen := now();
    END IF;
    IF (COALESCE(OLD.motor_ping, FALSE) IS DISTINCT FROM NEW.motor_ping) THEN
        NEW.motor_last_seen := now();
    END IF;

    -- Auto-OFF Safety (Tank Full)
    IF NEW.water_level = 100 AND NEW.motor_status = TRUE THEN
        NEW.motor_status    := FALSE;
        NEW.manual_override := FALSE;
        IF OLD.motor_status = FALSE THEN
            motor_rejected := TRUE;
            log_reason     := 'tank_full_rejection';
        ELSE
            log_reason := 'tank_full_auto_off';
        END IF;
    END IF;

    NEW.updated_at := now();

    -- Logging
    IF (OLD.motor_status IS DISTINCT FROM NEW.motor_status) OR (motor_rejected = TRUE) THEN
        IF NOT motor_rejected THEN
            IF NEW.motor_status = TRUE THEN
                log_reason := CASE WHEN NEW.mode IN ('STANDARD', 'LOCAL') THEN 'manual_on' ELSE 'remote_on' END;
            ELSE
                log_reason := CASE WHEN NEW.mode IN ('STANDARD', 'LOCAL') THEN 'manual_off' ELSE 'remote_off' END;
            END IF;
        END IF;

        INSERT INTO public.motor_logs (status, water_level, mode, reason)
        VALUES (NEW.motor_status, NEW.water_level, NEW.mode, log_reason);
    END IF;

    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS tr_motor_automation ON public.motor_system;
CREATE TRIGGER tr_motor_automation
BEFORE UPDATE ON public.motor_system
FOR EACH ROW EXECUTE FUNCTION public.handle_motor_automation();

-- ── 5. AUTOMATION LOGIC: AC ──────────────────────────────────

CREATE OR REPLACE FUNCTION public.handle_ac_automation()
RETURNS TRIGGER AS $$
DECLARE
    log_reason TEXT := 'system';
BEGIN
    -- Heartbeat Detection
    IF (COALESCE(OLD.ac_ping, FALSE) IS DISTINCT FROM NEW.ac_ping) THEN
        NEW.ac_last_seen := now();
    END IF;

    NEW.updated_at := now();

    -- Logging
    IF (OLD.ac_status IS DISTINCT FROM NEW.ac_status) THEN
        IF NEW.ac_status = TRUE THEN
            log_reason := CASE WHEN NEW.mode = 'LOCAL' THEN 'manual_on' ELSE 'remote_on' END;
        ELSE
            log_reason := CASE WHEN NEW.mode = 'LOCAL' THEN 'manual_off' ELSE 'remote_off' END;
        END IF;

        INSERT INTO public.ac_logs (status, mode, reason)
        VALUES (NEW.ac_status, NEW.mode, log_reason);
    END IF;

    RETURN NEW;
END;
$$ LANGUAGE plpgsql;

DROP TRIGGER IF EXISTS tr_ac_automation ON public.ac_system;
CREATE TRIGGER tr_ac_automation
BEFORE UPDATE ON public.ac_system
FOR EACH ROW EXECUTE FUNCTION public.handle_ac_automation();

-- ── 6. PERMISSIONS & REALTIME ───────────────────────────────

ALTER TABLE public.motor_system DISABLE ROW LEVEL SECURITY;
ALTER TABLE public.motor_logs DISABLE ROW LEVEL SECURITY;
ALTER TABLE public.motor_schedules DISABLE ROW LEVEL SECURITY;
ALTER TABLE public.ac_system DISABLE ROW LEVEL SECURITY;
ALTER TABLE public.ac_logs DISABLE ROW LEVEL SECURITY;
ALTER TABLE public.ac_schedules DISABLE ROW LEVEL SECURITY;

DO $$
BEGIN
    BEGIN
        ALTER PUBLICATION supabase_realtime ADD TABLE public.motor_system, public.motor_logs, public.motor_schedules, public.ac_system, public.ac_logs, public.ac_schedules;
    EXCEPTION WHEN OTHERS THEN 
        -- If already added, ignore
        NULL;
    END;
END $$;
