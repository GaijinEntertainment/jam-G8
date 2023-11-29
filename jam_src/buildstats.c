# include "buildstats.h"
# include "jam.h"

static unsigned long long* etaTable = NULL;
static int etaTableLen = 0;
static int etaTableCnt = 0;
static unsigned long long totalETA_msec = 0;
static unsigned long long totalTimeSpent_msec = 0;
static int updTargets = 0;

void etatab_set_hash(int idx, unsigned long long v)
{
    etaTable[idx*2] = v;
}

unsigned long long etatab_hash(int idx)
{
    return etaTable[idx*2];
}

unsigned long long* etatab_time(int idx)
{
    return &etaTable[idx*2+1];
}

void etatab_ensure_fits(int amount)
{
    if (etaTableLen <= amount)
    {
        etaTableLen = amount*2;
        etaTable = (unsigned long long*)realloc(etaTable, sizeof(unsigned long long)*etaTableLen*2);
    }
}

void bstat_load()
{
	etaTable = (unsigned long long*)malloc(sizeof(unsigned long long)*etaTableLen);

	FILE* etaf = fopen("jameta.bin", "rb+");
	if (etaf)
	{
	    fseek(etaf, 0, SEEK_END);
		etaTableCnt = ftell(etaf) / (sizeof(unsigned long long) * 2);
        etatab_ensure_fits(etaTableCnt);
		fseek(etaf, 0, SEEK_SET);
		int readed = fread(etaTable, sizeof(unsigned long long)*2, etaTableCnt, etaf);
        if (readed != etaTableCnt)
            etaTableCnt = readed > 0 ? readed : 0;
		fclose(etaf);
	}
	printf("...loaded %u ETA entries...\n", etaTableCnt);
}

void bstat_save()
{
    FILE* etablob = fopen("jameta.bin", "wb");
	if (etablob)
	{
		etaTableCnt = fwrite(etaTable, sizeof(unsigned long long)*2, etaTableCnt, etablob);
		fclose(etablob);
		printf("...saved %u ETA entries...\n", etaTableCnt);
	}
    free(etaTable);
}

unsigned long long* bstat_add_target(unsigned long long target_hash)
{
    for( int j = 0; j < etaTableCnt; j++ )
	{
	    if (etatab_hash(j) == target_hash)
        {
            totalETA_msec += *etatab_time(j);
		    return etatab_time(j);
        }
    }
    etatab_ensure_fits(etaTableCnt+1);
    etatab_set_hash(etaTableCnt, target_hash);
	etaTableCnt += 1;
	return etatab_time(etaTableCnt-1);		
}

void bstat_record_time_spent(unsigned long long* stat_ptr, unsigned long long time_msec)
{
    //FIXME: interlocked increment
    totalTimeSpent_msec += time_msec;
    if (stat_ptr)
        *stat_ptr = time_msec;
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
