#if !defined MXX_HEAD
#define MXX_HEAD

extern void nis_call_ecr(const char *fmt,...);
#define mxx_call_ecr( fmt, ...) nis_call_ecr( "[%s.%s] "fmt, __FILE__, __FUNCTION__, ##__VA_ARGS__)

extern int nis_getifmac(char *eth_name, unsigned char *pyhaddr);

#endif
