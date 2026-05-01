void attention(float *Q, float *K, float *V, float *out,
               int seq_len, int d_k, int d_v)
{
    float scores[64*64];
    for (int i = 0; i < seq_len; i++)
        for (int j = 0; j < seq_len; j++) {
            float s = 0.0f;
            for (int k = 0; k < d_k; k++)
                s += Q[i*d_k+k] * K[j*d_k+k];
            scores[i*seq_len+j] = s / __builtin_sqrtf((float)d_k);
        }
    for (int i = 0; i < seq_len; i++) {
        float mx = scores[i*seq_len];
        for (int j = 1; j < seq_len; j++)
            if (scores[i*seq_len+j] > mx) mx = scores[i*seq_len+j];
        float sum = 0.0f;
        for (int j = 0; j < seq_len; j++) {
            scores[i*seq_len+j] = __builtin_expf(scores[i*seq_len+j] - mx);
            sum += scores[i*seq_len+j];
        }
        for (int j = 0; j < seq_len; j++)
            scores[i*seq_len+j] /= sum;
    }
    for (int i = 0; i < seq_len; i++)
        for (int j = 0; j < d_v; j++) {
            float s = 0.0f;
            for (int k = 0; k < seq_len; k++)
                s += scores[i*seq_len+k] * V[k*d_v+j];
            out[i*d_v+j] = s;
        }
}
