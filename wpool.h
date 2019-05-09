#if !defined KE_H_20170118
#define KE_H_20170118

extern
int wp_init(int protocol);
extern
void wp_uninit(int protocol);
extern
int wp_queued(void *ncbptr);

#endif
