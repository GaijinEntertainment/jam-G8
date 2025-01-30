#include "buildstats.h"
#include "jam.h"

struct EtaTableEntry
{
  unsigned long long hash;
  unsigned long long time;
};

static struct EtaTableEntry *etaTable = NULL;
static int etaTableLen = 0;
static int etaTableCnt = 0;
static unsigned long long totalETA_msec = 0;
static unsigned long long totalTimeSpent_msec = 0;
static int updTargets = 0;
const int etaTableFields = 2;

void etatab_set_hash(int idx, unsigned long long v)
{
  etaTable[idx].hash = v;
}

unsigned long long etatab_hash(int idx)
{
  return etaTable[idx].hash;
}

unsigned long long *etatab_time(int idx)
{
  return &etaTable[idx].time;
}

void etatab_ensure_fits(int amount)
{
  if (etaTableLen <= amount)
  {
    int oldSz = etaTableLen;
    etaTableLen = amount * 2;
    etaTable = (struct EtaTableEntry*)realloc(etaTable, sizeof(struct EtaTableEntry) * etaTableLen);
    memset(etaTable + oldSz, 0, (etaTableLen - oldSz) * sizeof(struct EtaTableEntry));
  }
}

void bstat_load()
{
  FILE *etaf = fopen("jameta.bin", "rb+");
  if (etaf)
  {
    fseek(etaf, 0, SEEK_END);
    etaTableCnt = ftell(etaf) / (sizeof(struct EtaTableEntry));
    etatab_ensure_fits(etaTableCnt);
    fseek(etaf, 0, SEEK_SET);
    int readed = fread(etaTable, sizeof(struct EtaTableEntry), etaTableCnt, etaf);
    if (readed != etaTableCnt)
      etaTableCnt = readed > 0 ? readed : 0;
    fclose(etaf);
  }
  printf("...loaded %u ETA entries...\n", etaTableCnt);
}

void bstat_save()
{
  FILE *etablob = fopen("jameta.bin", "wb");
  if (etablob)
  {
    etaTableCnt = fwrite(etaTable, sizeof(struct EtaTableEntry), etaTableCnt, etablob);
    fclose(etablob);
    printf("...saved %u ETA entries...\n", etaTableCnt);
  }
  free(etaTable);
}

int bstat_add_target(unsigned long long target_hash)
{
  for (int j = 0; j < etaTableCnt; j++)
  {
    if (etatab_hash(j) == target_hash)
    {
      totalETA_msec += *etatab_time(j);
      return j;
    }
  }
  etatab_ensure_fits(etaTableCnt + 1);
  etatab_set_hash(etaTableCnt, target_hash);
  etaTableCnt += 1;
  return etaTableCnt - 1;
}

unsigned long long bstat_get_estimated_time(int id)
{
  if (id < 0)
    return 0;
  return *etatab_time(id);
}

void bstat_record_time_spent(int id, unsigned long long time_msec)
{
  if (id < 0)
    return;
  // FIXME: interlocked increment
  totalTimeSpent_msec += time_msec;
  *etatab_time(id) = time_msec;
}

unsigned long long bstat_get_total_eta_msec()
{
  return totalTimeSpent_msec >= totalETA_msec ? 0 : totalETA_msec - totalTimeSpent_msec;
}

void bstat_set_total_updating(int cnt)
{
  updTargets = cnt;
}

int bstat_get_total_updating()
{
  return updTargets;
}
