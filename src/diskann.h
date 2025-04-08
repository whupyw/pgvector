#include <stddef.h>
typedef enum diskann_metric_t
{
    DISKANN_L2,
    DISKANN_INNER_PRODUCT,
    DISKANN_COSINE
} diskann_metric_t;

const float *load_vector_data(const char *table_name, const char *column_name, size_t *npts, size_t *ndims);

void gen_random_slice(const float *inputdata, size_t npts, size_t ndims, double p_val, float **sampled_data, size_t *slice_size);

int build_disk_index(const char *dataFilePath, const char *indexFilePath,
                     const char *indexBuildParameters, enum diskann_metric_t compareMetric,
                     int use_opq, const char *codebook_prefix, const char *table_name, const char *column_name);
