void bstat_load();
void bstat_save();

int bstat_add_target(unsigned long long target_hash);
unsigned long long bstat_get_estimated_time(int id);

void bstat_record_time_spent(int id, unsigned long long time_msec);
unsigned long long bstat_get_total_eta_msec();

void bstat_set_total_updating(int cnt);
int bstat_get_total_updating();
