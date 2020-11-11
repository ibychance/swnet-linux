#if !defined MXX_HEAD
#define MXX_HEAD

extern void nis_call_ecr(const char *fmt,...);
#define mxx_call_ecr( fmt, arg...) nis_call_ecr( "[%s/%s] "fmt, __FILE__, __FUNCTION__, ##arg)

extern int nis_getifmac(char *eth_name, unsigned char *pyhaddr);

#endif
