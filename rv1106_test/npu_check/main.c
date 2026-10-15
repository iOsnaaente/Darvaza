// Confere a cadeia toda (toolchain, deploy, runtime e driver da NPU): carrega um
// modelo .rknn, mostra as versões do librknnmrt e do driver, os tensores e a
// memória do modelo, e mede o tempo de inferência com entrada zerada.
// Uso na placa: ./npu_check [modelo.rknn] [repetições]   (padrão: model/RetinaFace.rknn 20)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#include "rknn_api.h"

#define MAX_IO 16

static void *read_file(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *data = malloc(len);
    if (data && fread(data, 1, len, f) != (size_t)len) {
        free(data);
        data = NULL;
    }
    fclose(f);
    *size = (uint32_t)len;
    return data;
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void print_attr(const char *kind, const rknn_tensor_attr *a)
{
    printf("  %s %u %-24s [", kind, a->index, a->name);
    for (uint32_t i = 0; i < a->n_dims; i++)
        printf(i ? ",%u" : "%u", a->dims[i]);
    printf("] %s %s %s zp=%d scale=%g\n", get_format_string(a->fmt),
           get_type_string(a->type), get_qnt_type_string(a->qnt_type), a->zp, a->scale);
}

int main(int argc, char **argv)
{
    const char *model_path = argc > 1 ? argv[1] : "model/RetinaFace.rknn";
    int runs = argc > 2 ? atoi(argv[2]) : 20;

    struct utsname u;
    if (uname(&u) == 0)
        printf("kernel : %s %s %s\n", u.sysname, u.release, u.machine);

    uint32_t size = 0;
    void *model = read_file(model_path, &size);
    if (!model) {
        perror(model_path);
        return 1;
    }

    rknn_context ctx;
    int ret = rknn_init(&ctx, model, size, 0, NULL);
    free(model);
    if (ret < 0) {
        printf("rknn_init falhou: %d\n", ret);
        return 1;
    }

    rknn_sdk_version ver;
    if (rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &ver, sizeof(ver)) == RKNN_SUCC)
        printf("runtime: %s\ndriver : %s\n", ver.api_version, ver.drv_version);

    rknn_mem_size mem;
    if (rknn_query(ctx, RKNN_QUERY_MEM_SIZE, &mem, sizeof(mem)) == RKNN_SUCC)
        printf("memória: pesos %u KB, interna %u KB\n", mem.total_weight_size / 1024,
               mem.total_internal_size / 1024);

    rknn_input_output_num io;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    if (ret != RKNN_SUCC || io.n_input > MAX_IO || io.n_output > MAX_IO) {
        printf("rknn_query IN_OUT_NUM falhou: %d\n", ret);
        rknn_destroy(ctx);
        return 1;
    }
    printf("modelo : %s (%u entrada, %u saídas)\n", model_path, io.n_input, io.n_output);

    // O RV1106 só tem a API zero-copy: buffers da NPU no layout nativo, como nos
    // exemplos da Luckfox (entrada UINT8 NHWC, saídas NHWC).
    rknn_tensor_mem *mems[2 * MAX_IO] = {0};
    int n_mems = 0;
    for (uint32_t i = 0; i < io.n_input && ret == RKNN_SUCC; i++) {
        rknn_tensor_attr a = {.index = i};
        ret = rknn_query(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &a, sizeof(a));
        if (ret != RKNN_SUCC)
            break;
        a.type = RKNN_TENSOR_UINT8;
        a.fmt = RKNN_TENSOR_NHWC;
        print_attr("in ", &a);
        mems[n_mems] = rknn_create_mem(ctx, a.size_with_stride);
        if (!mems[n_mems]) {
            ret = RKNN_ERR_MALLOC_FAIL;
            break;
        }
        memset(mems[n_mems]->virt_addr, 0, a.size_with_stride);
        ret = rknn_set_io_mem(ctx, mems[n_mems++], &a);
    }
    for (uint32_t i = 0; i < io.n_output && ret == RKNN_SUCC; i++) {
        rknn_tensor_attr a = {.index = i};
        ret = rknn_query(ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &a, sizeof(a));
        if (ret != RKNN_SUCC)
            break;
        print_attr("out", &a);
        mems[n_mems] = rknn_create_mem(ctx, a.size_with_stride);
        if (!mems[n_mems]) {
            ret = RKNN_ERR_MALLOC_FAIL;
            break;
        }
        ret = rknn_set_io_mem(ctx, mems[n_mems++], &a);
    }

    if (ret == RKNN_SUCC)
        ret = rknn_run(ctx, NULL); // a primeira inclui a preparação
    if (ret == RKNN_SUCC && runs > 0) {
        double t0 = now_ms();
        for (int i = 0; i < runs && ret == RKNN_SUCC; i++)
            ret = rknn_run(ctx, NULL);
        if (ret == RKNN_SUCC)
            printf("inferência: %.2f ms (média de %d)\n", (now_ms() - t0) / runs, runs);
    }

    for (int i = 0; i < n_mems; i++)
        rknn_destroy_mem(ctx, mems[i]);
    rknn_destroy(ctx);
    if (ret != RKNN_SUCC) {
        printf("falhou: %d\n", ret);
        return 1;
    }
    printf("NPU OK\n");
    return 0;
}
