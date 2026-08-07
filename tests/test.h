#ifndef QTC_TEST_H
#define QTC_TEST_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ASSERT_TRUE(x) do { if (!(x)) { fprintf(stderr, "%s:%d assertion failed: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)
#define ASSERT_EQ_INT(a,b) do { long long _a=(long long)(a),_b=(long long)(b); if (_a!=_b) { fprintf(stderr, "%s:%d expected %lld == %lld\n",__FILE__,__LINE__,_a,_b); exit(1);} } while(0)
#define ASSERT_STREQ(a,b) do { if (strcmp((a),(b))!=0) { fprintf(stderr, "%s:%d expected \"%s\" == \"%s\"\n",__FILE__,__LINE__,(a),(b)); exit(1);} } while(0)
#endif
