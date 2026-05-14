#ifndef SPYRO_INTERP_REGIONS_H
#define SPYRO_INTERP_REGIONS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Register Spyro's render-side interpolation region table with the
 * openpete_interp ring. Call once from main() before openpete_run(). */
void spyro_interp_register(void);

#ifdef __cplusplus
}
#endif

#endif /* SPYRO_INTERP_REGIONS_H */
