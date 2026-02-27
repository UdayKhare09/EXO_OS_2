#ifndef _SYS_SYSMACROS_H
#define _SYS_SYSMACROS_H

#define major(dev) ((unsigned int)(((dev) >> 8) & 0xfffU))
#define minor(dev) ((unsigned int)(((dev) & 0xffU) | (((dev) >> 12) & 0xfffff00U)))
#define makedev(maj, min) ((((unsigned long)(min) & 0xffU) | (((unsigned long)(maj) & 0xfffU) << 8) | (((unsigned long)(min) & 0xfffff00U) << 12)))

#endif
