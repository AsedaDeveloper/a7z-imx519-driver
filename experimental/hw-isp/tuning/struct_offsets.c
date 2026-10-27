#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "isp_tuning_priv.h"
#define P(m) printf("%-22s off %6zu  size %6zu\n", #m, offsetof(struct isp_tunning_param, m), sizeof(((struct isp_tunning_param*)0)->m))
int main(void){
  printf("total %zu (file 99440)  USE_ENCPP=%d\n", sizeof(struct isp_tunning_param),
#ifdef USE_ENCPP
 1
#else
 0
#endif
  );
  P(plum_var); P(grad_th); P(dpc_remove_mode); P(ctc_th_max); P(bayer_gain); P(lsc_mode); P(lsc_tbl); P(lsc_trig_cfg); P(msc_mode); P(msc_blw_lut); P(msc_tbl); P(gamma_tbl_ini); P(gamma_trig_cfg);
  P(color_matrix_ini); P(ccm_trig_cfg); P(gca_cfg); P(pltm_cfg); P(sharp_comm_cfg); P(denoise_comm_cfg); P(tdf_comm_cfg); P(sensor_temp); P(isp_tdnf_df_shape); P(isp_sharp_ss_value);
  P(isp_sharp_hsv); P(isp_wdr_de_purpl_hsv_tbl); P(isp_pltm_stat_gd_cv); P(isp_pltm_df_cv); P(isp_cem_table); P(isp_pltm_lum_map_cv); P(isp_pltm_gtm_tbl);
  return 0; }
