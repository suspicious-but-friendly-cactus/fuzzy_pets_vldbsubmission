#ifndef PICNIC_CONFIG_H
#define PICNIC_CONFIG_H

/* from cmake checks */

/* #undef HAVE_SYS_AUXV_H */
/* #undef HAVE_ASM_HWCAP_H */
#define HAVE_SYS_RANDOM_H

#define HAVE_ALIGNED_ALLOC
#define HAVE_POSIX_MEMALIGN
/* #undef HAVE_MEMALIGN */
/* #undef HAVE_GETRANDOM */

#define HAVE_SECURITY_FRAMEWORK

#endif
