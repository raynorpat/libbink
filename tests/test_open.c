#include "bink.h"
#include "test_common.h"

int main(void)
{
    bink b = bink_open_file("/nonexistent/libbink-test.bik", BINK_MODE_DISK);

    CHECK(b == NULL);
    bink_close(NULL);
    return 0;
}
