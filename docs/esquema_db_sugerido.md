# Esquema DB Sugerido — Contrato V3

Documento ORIENTATIVO (no vinculante): la implementacion cloud puede variar,
pero DEBE cumplir el contrato normativo (`docs/contrato_nube_v3.md`), en
particular la deduplicacion por `client_id` y la aceptacion del esquema de
eventos.

## 1. Tabla `eventos` (obligatoria)

Tabla unica para todos los tipos de evento (decision D16). El firmware envia
arrays de filas con `POST <api_url>/eventos`.

```sql
create table eventos (
  client_id    bigint           not null,
  device_alias text             not null,
  event_type   text             not null,
  recorded_at  timestamptz      not null,
  -- reading
  zone_type    text             null,      -- 'substrate' | 'sprinkler'
  zone_index   integer          null,      -- 0-based
  reading_type text             null,      -- 'soil_humidity'|'soil_temp'|'air_temp'|'air_humidity'
  value        double precision null,      -- valor numerico
  -- irrigation_start / irrigation_stop
  trigger      integer          null,      -- 1=MANUAL 2=OVERRIDE 3=SCHEDULE 4=THRESHOLD
  duration_s   integer          null,
  -- alarm / alarm_cleared
  condition    integer          null,      -- 1..4
  -- command
  command_id   bigint           null,
  status       text             null,      -- 'accepted' | 'rejected'
  zone         text             null,      -- 'S0'..'S3' | 'A0'..'A1'
  action       text             null,      -- 'on' | 'off'
  created_at   timestamptz      not null default now()
);
```

### 1.1 Clave y deduplicacion

- `client_id` es monotono POR DISPOSITIVO, no global: la unicidad DEBE ser
  `(device_alias, client_id)`.
- Dedup idempotente (el firmware reenvia lotes tras fallos y reinicios):

```sql
alter table eventos
  add constraint eventos_pk primary key (device_alias, client_id);
```

- El `Prefer: resolution=ignore-duplicates` (PostgREST) convierte los
  conflictos de clave en "ignorar", que es exactamente la semantica del
  contrato.

### 1.2 Indices sugeridos

```sql
create index eventos_recording_time_idx on eventos (recorded_at desc);
create index eventos_device_idx        on eventos (device_alias, recorded_at desc);
create index eventos_type_idx          on eventos (event_type);
```

## 2. RLS

El firmware asume RLS desactivada (P6): la apikey del proyecto es la unica
credencial. La nube DEBE aceptar explicitamente este riesgo en el checklist de
entrega. Si en produccion se activa RLS, el equipo cloud DEBE configurar una
politica que permita INSERT con la apikey del proyecto y documentar el cambio
(no es parte del contrato).

## 3. Comandos (sugerencia, no vinculante)

El estado de los comandos viaja por WebSocket (`command_update`) y queda
registrado en `eventos` (filas `event_type='command'`). Si el equipo cloud
quiere una vista de comandos, puede derivarla:

```sql
create view comandos as
select device_alias, command_id, zone, action, duration_s, status, recorded_at
from eventos
where event_type = 'command';
```

## 4. Notas de tipo

- `recorded_at` llega en UTC sin fraccion (`YYYY-MM-DDTHH:MM:SSZ`); la nube
  DEBE interpretarlo como UTC (timestamptz lo hace automaticamente).
- `value` es decimal (porcentajes de humedad y temperaturas).
- Los campos de un tipo de evento no presentes en otros DEBEN ser `null`, no
  valores inventados.