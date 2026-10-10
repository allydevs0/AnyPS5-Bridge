#define STB_IMAGE_IMPLEMENTATION
#include "3rdparty/stb/stb_image.h"
#include <cstdio>

int main() {
    int w = 0, h = 0, c = 0;
    unsigned char* data = stbi_load("E:/anyps5-run/app0/splash_1.png", &w, &h, &c, 4);
    if (!data) {
        printf("stbi_load failed: %s\n", stbi_failure_reason());
    } else {
        printf("stbi_load succeeded! w=%d h=%d c=%d\n", w, h, c);
        stbi_image_free(data);
    }
    return 0;
}
