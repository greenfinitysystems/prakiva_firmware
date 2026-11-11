#include "core/prakiva_history.h"
#include "core/prakiva_data_acquisition.h"
#include "core/prakiva_time.h"
#include "core/prakiva_storage.h"
#include <LittleFS.h>
#include <stdlib.h>
#include <ArduinoJson.h>

namespace prakiva {
namespace {
constexpr char HISTORY_PATH[] = "/sensor_history.csv";
constexpr char TEMP_PATH[] = "/sensor_history.tmp";
constexpr char BACKUP_PATH[] = "/sensor_history.bak";
uint32_t aggregationWindowSeconds = 12; // Fast demo cadence from Product Specification.
size_t retentionPoints = 288;
constexpr size_t MAX_SENSORS = MAX_COMPONENT_INSTANCES;
struct Aggregate { double sum=0; size_t count=0; uint64_t lastTimestamp=0; };
String sensorIds[MAX_SENSORS];
Aggregate aggregates[MAX_SENSORS];
size_t sensorCount=0;
uint32_t nextWindow=0;
bool ready=false;
bool pending=false;
String pendingRow;
String expectedHeader;

String csvField(const String &line, size_t wanted) {
    size_t field=0, start=0;
    for (size_t i=0; i<=line.length(); ++i) {
        if (i==line.length() || line[i]==',') {
            if (field==wanted) return line.substring(start,i);
            ++field; start=i+1;
        }
    }
    return String();
}
size_t csvColumnCount(const String &line) { size_t n=1; for(size_t i=0;i<line.length();++i) if(line[i]==',') ++n; return n; }

bool ensureHistoryFile() {
    if (!LittleFS.exists(HISTORY_PATH)) {
        File f=LittleFS.open(HISTORY_PATH,"w");
        if(!f) return false;
        f.println(expectedHeader); f.close(); return true;
    }
    File f=LittleFS.open(HISTORY_PATH,"r");
    if(!f) return false;
    String header=f.readStringUntil('\n'); f.close(); header.trim();
    if(header==expectedHeader) return true;
    // A changed product sensor topology starts a new compatible history dataset.
    LittleFS.remove(BACKUP_PATH); LittleFS.remove(TEMP_PATH);
    if(!LittleFS.rename(HISTORY_PATH,BACKUP_PATH)) return false;
    File fresh=LittleFS.open(HISTORY_PATH,"w");
    if(!fresh) { LittleFS.rename(BACKUP_PATH,HISTORY_PATH); return false; }
    fresh.println(expectedHeader); fresh.close();
    Serial.println("[HISTORY] Sensor schema changed; started a new history dataset");
    return true;
}


bool repairHistoryRows() {
    File source=LittleFS.open(HISTORY_PATH,"r"); if(!source) return false;
    String header=source.readStringUntil('\n');
    LittleFS.remove(TEMP_PATH);
    File temp=LittleFS.open(TEMP_PATH,"w"); if(!temp) { source.close(); return false; }
    bool ok=temp.println(header)>0;
    while(source.available()) {
        String line=source.readStringUntil('\n'); line.trim();
        if(!line.length() || csvColumnCount(line)!=sensorCount+1) continue;
        const String ts=csvField(line,0);
        char *end=nullptr; const unsigned long stamp=strtoul(ts.c_str(),&end,10);
        if(end==ts.c_str() || *end!='\0' || stamp==0) continue;
        bool valid=true;
        for(size_t col=1;col<=sensorCount;++col) {
            const String value=csvField(line,col);
            if(!value.length()) continue;
            char *valueEnd=nullptr; const double numeric=strtod(value.c_str(),&valueEnd);
            if(valueEnd==value.c_str() || *valueEnd!='\0' || !isfinite(numeric)) { valid=false; break; }
        }
        if(valid && temp.println(line)==0) ok=false;
    }
    source.close(); temp.close();
    if(!ok) { LittleFS.remove(TEMP_PATH); return false; }
    LittleFS.remove(BACKUP_PATH);
    if(!LittleFS.rename(HISTORY_PATH,BACKUP_PATH)) { LittleFS.remove(TEMP_PATH); return false; }
    if(!LittleFS.rename(TEMP_PATH,HISTORY_PATH)) { LittleFS.rename(BACKUP_PATH,HISTORY_PATH); return false; }
    LittleFS.remove(BACKUP_PATH);
    return true;
}

bool appendWithRetention(const String &row) {
    File source=LittleFS.open(HISTORY_PATH,"r");
    if(!source) return false;
    String header=source.readStringUntil('\n');
    size_t rows=0;
    while(source.available()) { String line=source.readStringUntil('\n'); if(line.length()) ++rows; }
    source.close();
    if(rows < retentionPoints) {
        File out=LittleFS.open(HISTORY_PATH,"a"); if(!out) return false;
        const size_t written=out.println(row); out.close(); return written>0;
    }
    // Keep the newest retention-1 rows, then append the new row.
    source=LittleFS.open(HISTORY_PATH,"r"); if(!source) return false;
    source.readStringUntil('\n');
    const size_t skip=rows-(retentionPoints-1);
    for(size_t i=0;i<skip && source.available();++i) source.readStringUntil('\n');
    LittleFS.remove(TEMP_PATH);
    File temp=LittleFS.open(TEMP_PATH,"w"); if(!temp) { source.close(); return false; }
    bool ok=temp.println(header)>0;
    size_t copied=0;
    while(source.available()) { String line=source.readStringUntil('\n'); if(line.length()) { if(temp.println(line)==0) ok=false; ++copied; } }
    source.close();
    if(temp.println(row)==0) ok=false;
    temp.close();
    if(!ok) { LittleFS.remove(TEMP_PATH); return false; }
    LittleFS.remove(BACKUP_PATH);
    if(!LittleFS.rename(HISTORY_PATH,BACKUP_PATH)) { LittleFS.remove(TEMP_PATH); return false; }
    if(!LittleFS.rename(TEMP_PATH,HISTORY_PATH)) { LittleFS.rename(BACKUP_PATH,HISTORY_PATH); return false; }
    LittleFS.remove(BACKUP_PATH);
    return true;
}
}

bool beginHistoryService(ComponentRegistry &registry, const String &runtimeManifestJson) {
    if(!isStorageReady()) { Serial.println("[HISTORY] Disabled: storage unavailable"); return false; }
    JsonDocument manifest;
    if (deserializeJson(manifest, runtimeManifestJson) || !manifest["history_policy"].is<JsonObject>()) {
        Serial.println("[HISTORY] Disabled: Product Specification has no valid history_policy"); return false;
    }
    JsonObjectConst policy=manifest["history_policy"].as<JsonObjectConst>();
    if (!(policy["enabled"] | false)) { Serial.println("[HISTORY] Disabled by Product Specification"); return false; }
    aggregationWindowSeconds=policy["aggregation_window_seconds"] | 12U;
    retentionPoints=policy["retention_points"] | 288U;
    if(aggregationWindowSeconds<2 || aggregationWindowSeconds>86400 || retentionPoints==0 || retentionPoints>288) {
        Serial.println("[HISTORY] Disabled: invalid history policy values"); return false;
    }
    sensorCount=0; expectedHeader="timestamp";
    for(size_t i=0;i<registry.instanceCount() && sensorCount<MAX_SENSORS;++i) {
        const char *id=registry.componentIdAt(i); IComponent *c=id?registry.find(id):nullptr;
        if(!c || !c->asSensorDriver()) continue;
        sensorIds[sensorCount]=id; expectedHeader += ","; expectedHeader += sensorIds[sensorCount]; ++sensorCount;
    }
    if(sensorCount==0) { Serial.println("[HISTORY] No sensors registered"); return false; }
    // Recover interrupted compaction before validating the active CSV.
    if(!LittleFS.exists(HISTORY_PATH) && LittleFS.exists(BACKUP_PATH)) LittleFS.rename(BACKUP_PATH,HISTORY_PATH);
    LittleFS.remove(TEMP_PATH);
    if(!ensureHistoryFile() || !repairHistoryRows()) { Serial.println("[HISTORY] Could not prepare/recover CSV file"); return false; }
    uint32_t now=0;
    if(getCurrentUnixTime(now)) nextWindow=((now/aggregationWindowSeconds)+1)*aggregationWindowSeconds;
    else nextWindow=0;
    for(size_t i=0;i<sensorCount;++i) aggregates[i]=Aggregate{};
    pending=false; ready=true;
    Serial.printf("[HISTORY] Ready: sensors=%u window=%us retention=%u\n",static_cast<unsigned>(sensorCount),static_cast<unsigned>(aggregationWindowSeconds),static_cast<unsigned>(retentionPoints));
    return true;
}

void historyRecord() {
    if(!ready || pending) return;
    uint32_t now=0; if(!getCurrentUnixTime(now)) return;
    if(nextWindow==0) nextWindow=((now/aggregationWindowSeconds)+1)*aggregationWindowSeconds;
    for(size_t i=0;i<sensorCount;++i) {
        const SensorSnapshotEntry *entry=findSensorSnapshot(sensorIds[i].c_str());
        if(!entry || !entry->readSucceeded || !entry->reading.valid || !entry->reading.value.isValid()) continue;
        const uint64_t stamp=entry->reading.timestamp;
        if(stamp==0 || stamp==aggregates[i].lastTimestamp) continue;
        const double value=entry->reading.value.numericValue.toDouble();
        if(!isfinite(value)) continue;
        aggregates[i].sum+=value; ++aggregates[i].count; aggregates[i].lastTimestamp=stamp;
    }
    if(now<nextWindow) return;
    String row=String(nextWindow);
    for(size_t i=0;i<sensorCount;++i) {
        row += ",";
        if(aggregates[i].count) row += String(aggregates[i].sum/static_cast<double>(aggregates[i].count),4);
    }
    pendingRow=row; pending=true;
    for(size_t i=0;i<sensorCount;++i) aggregates[i]=Aggregate{};
    nextWindow += aggregationWindowSeconds;
    // If the loop was delayed across multiple windows, skip empty backfill rows.
    while(nextWindow<=now) nextWindow+=aggregationWindowSeconds;
}

void historyFlush() {
    if(!ready || !pending) return;
    if(appendWithRetention(pendingRow)) {
        pending=false; pendingRow="";
    } else Serial.println("[HISTORY] WARNING: failed to persist completed history row; will retry");
}

bool appendSensorHistoryInternal(const char *componentId, size_t limit, JsonArray output) {
    if(!ready || !componentId || !componentId[0]) return false;
    size_t column=MAX_SENSORS+1;
    for(size_t i=0;i<sensorCount;++i) if(sensorIds[i]==componentId) { column=i+1; break; }
    if(column>sensorCount) return false;
    if(limit==0 || limit>retentionPoints) limit=retentionPoints;
    if(limit>288) limit=288;
    File f=LittleFS.open(HISTORY_PATH,"r"); if(!f) return false;
    f.readStringUntil('\n');
    size_t totalRows=0;
    while(f.available()) { String line=f.readStringUntil('\n'); if(line.length()) ++totalRows; }
    f.close();
    const size_t skipRows=totalRows>limit?totalRows-limit:0;
    f=LittleFS.open(HISTORY_PATH,"r"); if(!f) return false;
    f.readStringUntil('\n');
    size_t skipped=0;
    while(f.available() && skipped<skipRows) { String line=f.readStringUntil('\n'); if(line.length()) ++skipped; }
    while(f.available()) {
        String line=f.readStringUntil('\n'); line.trim(); if(!line.length()) continue;
        JsonObject point=output.add<JsonObject>();
        const String timestamp=csvField(line,0);
        const String value=csvField(line,column);
        point["timestamp"]=strtoul(timestamp.c_str(),nullptr,10);
        if(value.length()) point["value"]=value.toDouble(); else point["value"]=nullptr;
    }
    f.close();
    return true;
}
HistoryService history;
bool HistoryService::begin(ComponentRegistry &registry, const String &manifest) { return beginHistoryService(registry, manifest); }
void HistoryService::record() { historyRecord(); }
void HistoryService::flush() { historyFlush(); }
bool HistoryService::appendSensorHistory(const char *componentId, size_t limit, JsonArray output) { return appendSensorHistoryInternal(componentId, limit, output); }

}
