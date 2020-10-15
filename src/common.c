/*******************************************************************************
    Collective Matrix Factorization
    -------------------------------
    
    This is a module for multi-way factorization of sparse and dense matrices
    intended to be used for recommender system with explicit feedback data plus
    side information about users and/or items.

    The reference papers are:
        (a) Cortes, David.
            "Cold-start recommendations in Collective Matrix Factorization."
            arXiv preprint arXiv:1809.00366 (2018).
        (b) Singh, Ajit P., and Geoffrey J. Gordon.
            "Relational learning via collective matrix factorization."
            Proceedings of the 14th ACM SIGKDD international conference on
            Knowledge discovery and data mining. 2008.
        (c) Hu, Yifan, Yehuda Koren, and Chris Volinsky.
            "Collaborative filtering for implicit feedback datasets."
            2008 Eighth IEEE International Conference on Data Mining.
            Ieee, 2008.
        (d) Takacs, Gabor, Istvan Pilaszy, and Domonkos Tikk.
            "Applications of the conjugate gradient method for
            implicit feedback collaborative filtering."
            Proceedings of the fifth ACM conference on
            Recommender systems. 2011.

    For information about the models offered here and how they are fit to
    the data, see the files 'collective.c' and 'offsets.c'.

    Written for C99 standard and OpenMP version 2.0 or higher, and aimed to be
    used either as a stand-alone program, or wrapped into scripting languages
    such as Python and R.
    <https://www.github.com/david-cortes/cmfrec>

    

    MIT License:

    Copyright (c) 2020 David Cortes

    All rights reserved.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to
    deal in the Software without restriction, including without limitation the
    rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
    sell copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in
    all copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
    IN THE SOFTWARE.

*******************************************************************************/

#include "cmfrec.h"

/*******************************************************************************
    Function and Gradient for cannonical form
    -----------------------------------------

    This function computes the gradients in the cannonical-form problem:
        min  0.5 * scaling * ||M . W . (X - A*t(B) - bias1 - bias2 - mu)||^2
    (Note that it will not add regularization into the formula)
    See the files "collective.c" and "offsets.c" for details on what the
    formula represents.

    The gradients are given as:
        E = scaling * (M . W . (A*t(B) - X))
        grad(bias1) = sum_cols(E)
        grad(bias2) = sum_rows(E)
        grad(A) =   E  * B
        grad(B) = t(E) * A

    Since the formulas here ignore missing entries, when the matrices are
    sparse, the gradients can be calculated more easily as follows:
        grad(A) := 0
        grad(B) := 0
        For i..nnz:
            err = <A[row], B[col]> - X[row,col]
            grad(A[row]) += B[col] * err
            grad(B[col]) += A[row] * err

    In order to speed things up, this loop can be parallelized in two ways:
    - Let each thread/worker have separate matrices grad(A), grad(B), to which
      each adds its own portions based on the non-zero entries it is assigned,
      then sum them up into a common matrix (this part can also be parallelized
      by rows). This approach is the fastest, but letting each thread/worker
      have a full copy of grad(A), grad(B) can require a lot of memory.
    - Have copies of X in CSR and CSC formats, then iterate **twice** over it:
      (a) first by rows (using the CSR matrix), letting each thread write into
          a different row at a time (no race conditions).
      (b) then by columns (using the CSC matrix), letting each thread write into
         a different column at a time.
      This approach is slower, as it requires iterating twice, thus computing
      twice as many dot products, but it requires less memory as each thread
      writes into the final matrices grad(A), grad(B).

    This module implements both approaches, but it is suggested to use the
    first one if memory constraints allow for it.

    If passing a dense matrix X as input, need to pass a temporary buffer
    of dimensions m * n regardless.

    Note that the arrays for the gradients must be passed already initialized:
    that is, they must be set to all-zeros to obtain the full gradient, or to
    the already-obtained gradients from the main factorization if using it for
    the collective model.

    Parameters
    ----------
    A[m * lda]
        Array with the A parameters. Only the portion A(:m,:k) will be taken.
    lda
        Leading dimension of the array A (>= k).
    B[n * ldb]
        Array with the B parameters. Only the portion B(:n,:k) will be taken.
    ldb
        Leading dimension of the array B (>= k).
    g_A[m * lda], g_B[n * ldb] (out)
        Arrays to which to sum the computed gradients for A and B.
    m
        Number of rows in X.
    n
        Number of columns in X.
    k
        Dimensionality of the A and B matries (a.k.a. latent factors)
    ixA[nnz], ixB[nnz], X[nnz], nnz
        Matrix X in triplets format (row,col,x). Should only pass one of
        'X', 'Xfull', 'Xcsr/Xcsc'. If 'Xfull' is passed, it will be preferred.
        Pass NULL if 'X' will be passed in a different format.
    Xfull[m * n]
        Matrix X in dense form, with missing entries as NAN. Should only pass
        one of 'X', 'Xfull', 'Xcsr/Xcsc'. If 'Xfull' is passed, it will be
        preferred. Pass NULL if 'X' will be passed in a different format.
    full_dense
        Whether the X matrix contains no missing values.
    Xcsr_p[m+1], Xcsr_i[nnz], Xcsr[nnz], Xcsc_p[n], Xcsc_i[nnz], Xcsc[nnz]
        Matrix X in both CSR and CSC formats, in wich array 'p' indicates the
        starting position of a row for CSR and column for CSC in the Xcsr array,
        and array 'i' indicates the corresponding column for CSR and row for CSC
        for that entry. Should only pass one of 'X', 'Xfull', 'Xcsr/Xcsc'. If
        'Xfull' is passed, it will be preferred. Pass NULL if 'X' will be passed
         in a different format.
    user_bias, item_bias
        Whether user/row and/or item/column biases are to be used. Ignored if
        passing overwrite_grad' = 'false'
    biasA[m], biasB[n]
        Arrays containing the row/column biases. Pass NULL if not used.
    g_biasA[m], g_biasB[n] (out)
        Arrays in which to sum the gradient calculations for the biases.
    weight[nnz or m*n], weightR[nnz], weightC[nnz]
        Observation weights for X. Must match the shape of X - that is, either
        'nnz' for sparse, or 'm*n' for dense. If passing CSR/CSC matrices for X,
        must pass these in variables 'weightR' and 'weightC', otherwise, should
        be passed in 'weight'. Pass NULL if not used.
    scaling
        Scaling to add to the objective function. Pass '1' for no scaling.
    buffer_FPnum[m*n]
        If passing a dense matrix, temporary array which will be overwritten.
        Not required for sparse matrices.
    buffer_mt[nthreads * k * (m+n+1)]
        If passing X and nthreads > 1, will be used as a temporary array into
        which to copy thread-local values for the one-pass parallelization
        strategy described at the top. Not required if passing Xcsr/Xcsc,
        Xfull, or nthreads = 1.
    overwrite_grad
        Whether it is allowed to overwrite the gradient arrays. If passing
        'false', will ignore the biases. If passing 'true', will assume that
        all the gradient arrays are continuous in memory. The reasoning behind
        is to allow for a more numerically precise calculation when there is
        some scaling parameter.
    nthreads
        Number of parallel threads to use. Note that BLAS and LAPACK threads
        are not controlled through this function.

    Returns
    -------
    f : The evaluated function value.


*******************************************************************************/

FPnum fun_grad_cannonical_form
(
    FPnum *restrict A, int lda, FPnum *restrict B, int ldb,
    FPnum *restrict g_A, FPnum *restrict g_B,
    int m, int n, int k,
    int ixA[], int ixB[], FPnum *restrict X, size_t nnz,
    FPnum *restrict Xfull, bool full_dense,
    size_t Xcsr_p[], int Xcsr_i[], FPnum *restrict Xcsr,
    size_t Xcsc_p[], int Xcsc_i[], FPnum *restrict Xcsc,
    bool user_bias, bool item_bias,
    FPnum *restrict biasA, FPnum *restrict biasB,
    FPnum *restrict g_biasA, FPnum *restrict g_biasB,
    FPnum *restrict weight, FPnum *restrict weightR, FPnum *restrict weightC,
    FPnum scaling,
    FPnum *restrict buffer_FPnum,
    FPnum *restrict buffer_mt,
    bool overwrite_grad,
    int nthreads
)
{
    /* TODO: 'overwrite_grad' is no longer used, should remove that code */
    #if defined(_OPENMP) && \
                ( (_OPENMP < 200801)  /* OpenMP < 3.0 */ \
                  || defined(_WIN32) || defined(_WIN64) \
                )
    long long ix, ia, ib;
    #else
    size_t ia = 0, ib = 0;
    #endif

    double f = 0;
    double corr = 0;
    double tempf = 0;
    double fsum = 0;

    /* these will be de-referenced, but not updated */
    if (!user_bias)
    {
        if (!item_bias)
            g_biasA = g_A;
        else if (g_A == g_biasB + n)
            g_biasA = g_biasB;
        else
            g_biasA = g_A;
    }

    if (!item_bias)
    {
        if (user_bias && (g_A == g_biasA + m || g_biasA == g_A))
            g_biasB = g_biasA;
        else if (!user_bias &&
                 g_B == g_A + (size_t)m*(size_t)lda - (size_t)(lda-k))
            g_biasB = g_A;
        else
            g_biasB = g_B;
    }

    FPnum err;
    size_t m_by_n = (size_t)m * (size_t)n;

    bool parallel_onepass = (Xfull == NULL && nthreads > 1 &&
                             Xcsr == NULL  && buffer_mt != NULL);
    if (parallel_onepass)
        set_to_zero(buffer_mt, (size_t)nthreads
                                * (  (size_t)(k + (int)user_bias) * (size_t)m
                                    +(size_t)(k + (int)item_bias) * (size_t)n),
                    nthreads);

    if (Xfull == NULL)  /* sparse input with NAs - this is the expected case */
    {
        /* This is the loop as explained at the top.
           Note about the differentiation here: if using it for the main
           factorization (which allows biases), it's not a problem to overwrite
           the gradient matrices over their full leading dimension, and these
           are expected to be all continuous in a memory layout. When there is
           some scaling applied, it is more numerically  precise to apply the
           scaling after all the variables have already been summed, but if the
           gradients already have some information from a previous factorization
           to which these should add instead of overwrite them, it's necessary
           to apply the scaling observation-by-observation instead */
        if (!overwrite_grad)
        {
            #ifdef _OPENMP
            if (    nthreads <= 1 ||
                    (Xcsr == NULL && !parallel_onepass) ||
                    (Xcsr != NULL && nthreads <= 2) )
            #endif
            {
                for (size_t ix = 0; ix < nnz; ix++)
                {
                    ia = (size_t)ixA[ix];
                    ib = (size_t)ixB[ix];
                    err = cblas_tdot(k, A + ia*(size_t)lda, 1,
                                     B + ib*(size_t)ldb, 1)
                           - X[ix];

                    tempf = square(err)*((weight==NULL)? 1. : weight[ix])-corr;
                    fsum = f + tempf;
                    corr = (fsum - f) - tempf;
                    f = fsum;

                    err *= scaling * ((weight == NULL)? 1. : weight[ix]);

                    cblas_taxpy(k, err, B + ib*(size_t)ldb, 1,
                                g_A + ia*(size_t)lda, 1);
                    cblas_taxpy(k, err, A + ia*(size_t)lda, 1,
                                g_B + ib*(size_t)ldb, 1);
                }
            }
            #ifdef _OPENMP
            else if (parallel_onepass)
            {
                size_t thr_szA = (size_t)m*(size_t)k;
                size_t thr_szB = (size_t)n*(size_t)k;
                FPnum *restrict g_A_t = buffer_mt;
                FPnum *restrict g_B_t = g_A_t + (size_t)nthreads*thr_szA;

                #pragma omp parallel for schedule(static) num_threads(nthreads)\
                        reduction(+:f) private(ia, ib, err) \
                        shared(ixA, ixB, X, nnz, A, B, lda, ldb, k, weight, \
                               scaling, thr_szA, thr_szB)
                for (size_t_for ix = 0; ix < nnz; ix++)
                {
                    ia = (size_t)ixA[ix];
                    ib = (size_t)ixB[ix];
                    err = cblas_tdot(k, A + ia*(size_t)lda, 1,
                                     B + ib*(size_t)ldb, 1)
                           - X[ix];

                    f += square(err)*((weight == NULL)? 1. : weight[ix]);
                    err *= scaling * ((weight == NULL)? 1. : weight[ix]);

                    cblas_taxpy(k, err, B + ib*(size_t)ldb, 1,
                                g_A_t + (size_t)(omp_get_thread_num())*thr_szA
                                      + ia*(size_t)lda, 1);
                    cblas_taxpy(k, err, A + ia*(size_t)lda, 1,
                                g_B_t + (size_t)(omp_get_thread_num())*thr_szB
                                      + ib*(size_t)ldb, 1);
                }

                reduce_mat_sum(g_A, lda, g_A_t,
                               m, k, nthreads);
                reduce_mat_sum(g_B, ldb, g_B_t,
                               n, k, nthreads);
            }

            else
            {
                #pragma omp parallel for schedule(dynamic) \
                        num_threads(nthreads) \
                        private(err, ib, tempf) reduction(+:f) \
                        shared(m, k, A, B, lda, ldb, Xcsr, Xcsr_p, Xcsr_i, \
                               scaling, weightR, g_A)
                for (size_t_for ia = 0; ia < (size_t)m; ia++)
                {
                    tempf = 0;
                    for (size_t ix = (size_t)Xcsr_p[ia];
                         ix < (size_t)Xcsr_p[ia+(size_t)1]; ix++)
                    {
                        ib = (size_t)Xcsr_i[ix];
                        err = cblas_tdot(k, A + ia*(size_t)lda, 1,
                                         B + ib*(size_t)ldb, 1)
                               - Xcsr[ix];

                        tempf += square(err)
                                  * ((weightR == NULL)? 1. : weightR[ix]);
                        err *= scaling * ((weightR == NULL)? 1. : weightR[ix]);

                        cblas_taxpy(k, err, B + ib*(size_t)ldb, 1,
                                    g_A + ia*(size_t)lda, 1);
                    }
                    f += tempf;
                }

                #pragma omp parallel for schedule(dynamic) \
                        num_threads(nthreads) private(err, ia) \
                        shared(n, k, A, B, lda, ldb, Xcsc, Xcsc_p, Xcsc_i, \
                               scaling, weightC, g_B)
                for (size_t_for ib = 0; ib < (size_t)n; ib++)
                {
                    for (size_t ix = (size_t)Xcsc_p[ib];
                         ix < (size_t)Xcsc_p[ib+(size_t)1]; ix++)
                    {
                        ia = (size_t)Xcsc_i[ix];
                        err = cblas_tdot(k, A + ia*(size_t)lda, 1,
                                         B + ib*(size_t)ldb, 1)
                               - Xcsc[ix];
                        err *= scaling * ((weightC == NULL)? 1. : weightC[ix]);

                        cblas_taxpy(k, err, A + ia*(size_t)lda, 1,
                                    g_B + ib*(size_t)ldb, 1);
                    }
                }

            }
            #endif
        }

        else /* overwrite_grad */
        {
            #ifdef _OPENMP
            if (    nthreads <= 1 ||
                    (Xcsr == NULL && !parallel_onepass) ||
                    (Xcsr != NULL && nthreads <= 2) )
            #endif
            {
                for (size_t ix = 0; ix < nnz; ix++)
                {
                    ia = (size_t)ixA[ix];
                    ib = (size_t)ixB[ix];
                    err = cblas_tdot(k, A + ia*(size_t)lda, 1,
                                     B + ib*(size_t)ldb, 1)
                           - X[ix];
                    err += (user_bias? biasA[ia] : 0.)
                         + (item_bias? biasB[ib] : 0.);

                    tempf = square(err)*((weight==NULL)? 1. : weight[ix])-corr;
                    fsum = f + tempf;
                    corr = (fsum - f) - tempf;
                    f = fsum;

                    err *= ((weight == NULL)? 1. : weight[ix]);

                    g_biasA[ia] += user_bias? err : 0.;
                    g_biasB[ib] += item_bias? err : 0.;
                    cblas_taxpy(k, err, B + ib*(size_t)ldb, 1,
                                g_A + ia*(size_t)lda, 1);
                    cblas_taxpy(k, err, A + ia*(size_t)lda, 1,
                                g_B + ib*(size_t)ldb, 1);
                }
            }

            #ifdef _OPENMP
            else if (parallel_onepass)
            {
                size_t thr_szA = (size_t)m*(size_t)k;
                size_t thr_szB = (size_t)n*(size_t)k;
                FPnum *restrict g_A_t = buffer_mt;
                FPnum *restrict g_B_t = g_A_t + (size_t)nthreads*thr_szA;
                FPnum *restrict g_biasA_t = g_B_t + (size_t)nthreads*thr_szB;
                FPnum *restrict g_biasB_t = g_biasA_t
                                             + (user_bias?
                                                ((size_t)nthreads * (size_t)m)
                                                : (0));

                #pragma omp parallel for schedule(static) num_threads(nthreads)\
                        reduction(+:f) private(ia, ib, err) \
                        shared(ixA, ixB, X, nnz, A, B, lda, ldb, k, weight, \
                               scaling, thr_szA, thr_szB, g_biasA_t, g_biasB_t,\
                               biasA, biasB, user_bias, item_bias, m, n)
                for (size_t_for ix = 0; ix < nnz; ix++)
                {
                    ia = (size_t)ixA[ix];
                    ib = (size_t)ixB[ix];
                    err = cblas_tdot(k, A + ia*(size_t)lda, 1,
                                     B + ib*(size_t)ldb, 1)
                           + (user_bias? biasA[ia] : 0.)
                           + (item_bias? biasB[ib] : 0.)
                           - X[ix];

                    f += square(err) * ((weight == NULL)? 1. : weight[ix]);
                    err *= ((weight == NULL)? 1. : weight[ix]);

                    g_biasA_t[ia + (size_t)m*(size_t)(omp_get_thread_num())]
                        += user_bias? err : 0.;
                    g_biasB_t[ib + (size_t)n*(size_t)(omp_get_thread_num())]
                        += item_bias? err : 0.;

                    cblas_taxpy(k, err, B + ib*(size_t)ldb, 1,
                                g_A_t + (size_t)(omp_get_thread_num())*thr_szA
                                      + ia*(size_t)lda, 1);
                    cblas_taxpy(k, err, A + ia*(size_t)lda, 1,
                                g_B_t + (size_t)(omp_get_thread_num())*thr_szB
                                      + ib*(size_t)ldb, 1);
                }

                reduce_mat_sum(g_A, lda, g_A_t,
                               m, k, nthreads);
                reduce_mat_sum(g_B, ldb, g_B_t,
                               n, k, nthreads);
                if (user_bias)
                    reduce_mat_sum(g_biasA, 1, g_biasA_t,
                                   m, 1, nthreads);
                if (item_bias)
                    reduce_mat_sum(g_biasB, 1, g_biasB_t,
                                   n, 1, nthreads);
            }

            else
            {
                #pragma omp parallel for schedule(dynamic) reduction(+:f) \
                        num_threads(nthreads) private(err, ib, tempf) \
                        shared(m, k, A, B, lda, ldb, Xcsr, Xcsr_p, Xcsr_i, \
                               scaling, weightR, g_A, user_bias, item_bias, \
                               biasA, biasB, g_biasA)
                for (ia = 0; ia < (size_t)m; ia++)
                {
                    tempf = 0;
                    for (size_t ix = (size_t)Xcsr_p[ia];
                                ix < (size_t)Xcsr_p[ia+(size_t)1]; ix++)
                    {
                        ib = (size_t)Xcsr_i[ix];
                        err = cblas_tdot(k, A + ia*(size_t)lda, 1,
                                         B + ib*(size_t)ldb, 1)
                             + (user_bias? biasA[ia] : 0.)
                             + (item_bias? biasB[ib] : 0.)
                             - Xcsr[ix];

                        tempf += square(err)
                                  * ((weightR == NULL)? 1. : weightR[ix]);
                        err *= ((weightR == NULL)? 1. : weightR[ix]);

                        g_biasA[ia] += user_bias? err : 0.;
                        cblas_taxpy(k, err, B + ib*(size_t)ldb, 1,
                                    g_A + ia*(size_t)lda, 1);
                    }
                    f += tempf;
                }


                #pragma omp parallel for schedule(dynamic) \
                        num_threads(nthreads) private(err, ia) \
                        shared(n, k, A, B, lda, ldb, Xcsc, Xcsc_p, Xcsc_i, \
                               scaling, weightC, g_B, user_bias, item_bias, \
                               biasA, biasB, g_biasB)
                for (ib = 0; ib < (size_t)n; ib++)
                {
                    for (size_t ix = (size_t)Xcsc_p[ib];
                                ix < (size_t)Xcsc_p[ib+(size_t)1]; ix++)
                    {
                        ia = (size_t)Xcsc_i[ix];
                        err = cblas_tdot(k, A + ia*(size_t)lda, 1,
                                         B + ib*(size_t)ldb, 1)
                               + (user_bias? biasA[ia] : 0.)
                               + (item_bias? biasB[ib] : 0.)
                               - Xcsc[ix];
                        err *= ((weightC == NULL)? 1. : weightC[ix]);

                        g_biasB[ib] += item_bias? err : 0.;
                        cblas_taxpy(k, err, A + ia*(size_t)lda, 1,
                                    g_B + ib*(size_t)ldb, 1);
                    }
                }
            }
            #endif

            #pragma omp barrier
            if (scaling != 1.)
            {
                /* Note: the gradients should be contiguous in memory in the
                   following order: biasA, biasB, A, B - hence these conditions.
                   If passing discontiguous arrays (e.g. when the gradient is
                   a zeroed-out array and later summed to the previous
                   gradient), there should be no biases, and the biases
                   should already be assigned to the same memory locations as
                   the gradient arrays. Otherwise should pass
                   'overwrite_grad=false', which should not used within this
                   module */
                if (g_B == g_biasA
                                + (size_t)(user_bias? m : 0)
                                + (size_t)(item_bias? n : 0)
                                + ((size_t)m*(size_t)lda  - (size_t)(lda-k)))
                {
                    tscal_large(g_biasA, scaling,
                                ((size_t)m*(size_t)lda + (size_t)n*(size_t)ldb)
                                + (size_t)(user_bias? m : 0)
                                + (size_t)(item_bias? n : 0)
                                - (size_t)(lda-k),
                                nthreads);
                }

                else if (!user_bias && !item_bias &&
                         g_B == g_A + (size_t)m*(size_t)lda- (size_t)(lda-k))
                {
                    tscal_large(g_A, scaling,
                                ((size_t)m*(size_t)lda + (size_t)n*(size_t)ldb),
                                nthreads);
                }

                else
                {
                    if (user_bias)
                        cblas_tscal(m, scaling, g_biasA, 1);
                    if (item_bias)
                        cblas_tscal(n, scaling, g_biasB, 1);
                    tscal_large(g_A, scaling,
                                ((size_t)m*(size_t)lda) - (size_t)(lda-k),
                                nthreads);
                    tscal_large(g_B, scaling,
                                ((size_t)n*(size_t)ldb) - (size_t)(ldb-k),
                                nthreads);
                }
            }
        }
    }

    else /* dense input - this is usually not optimal, but still supported */
    {
        /* Buffer = X */
        copy_arr(Xfull, buffer_FPnum, m_by_n, nthreads);
        /* Buffer = A*t(B) - Buffer */
        cblas_tgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                    m, n, k,
                    1., A, lda, B, ldb,
                    -1., buffer_FPnum, n);
        /* Buffer += biasA[m,1] + biasB[1,n] */
        if (user_bias)
            mat_plus_rowvec(buffer_FPnum, biasA, m, n, nthreads);
        if (item_bias)
            mat_plus_colvec(buffer_FPnum, biasB, 1., m, n, (size_t)n, nthreads);

        /* Buffer *= W  (Now buffer becomes E without the scaling) */
        if (full_dense) {
            if (weight != NULL)
                mult_elemwise(buffer_FPnum, weight, m_by_n, nthreads);
        } else {
            if (weight == NULL)
                nan_to_zero(buffer_FPnum, Xfull, m_by_n, nthreads);
            else
                mult_if_non_nan(buffer_FPnum, Xfull, weight, m_by_n, nthreads);
        }

        /* f = ||E||^2 */
        if (weight == NULL)
            f = sum_squares(buffer_FPnum, m_by_n, nthreads);
        else
            f = sum_sq_div_w(buffer_FPnum, weight, m_by_n, true, nthreads);

        /* grad(bias1) = scaling * sum_rows(E) */
        if (user_bias) {
            sum_by_rows(buffer_FPnum, g_biasA, m, n, nthreads);
            if (scaling != 1)
                cblas_tscal(m, scaling, g_biasA, 1);
        }
        /* grad(bias2) = scaling * sum_cols(E) */
        if (item_bias) {
            sum_by_cols(buffer_FPnum, g_biasB, m, n, (size_t)n, nthreads);
            if (scaling != 1)
                cblas_tscal(n, scaling, g_biasB, 1);
        }

        /* grad(A) =  scaling * E * B */
        cblas_tgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    m, k, n,
                    scaling, buffer_FPnum, n, B, ldb,
                    overwrite_grad? 0. : 1., g_A, lda);
        /* grad(B) = scaling * t(E) * A */
        cblas_tgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    n, k, m,
                    scaling, buffer_FPnum, n, A, lda,
                    overwrite_grad? 0. : 1., g_B, ldb);
        /* Note: don't apply the scaling earlier as otherwise it
           loses precision, even if it manages to save some operations */
    }

    return (FPnum)((scaling / 2.) * f);
}

/*******************************************************************************
    Closed-form solution for cannonical form
    ----------------------------------------

    This function uses the closed form to obtain the least-squares minimizer
    for a single row of the A matrix:
        min ||M . W . (X - A*t(B)) ||^2

    The formula is given as follows:
        Aopt[1,k] = inv(t(B'*W) * B' + diag(lambda)) * (t(B' * W) * Xa[1,n])

    Where B'[n,k] is a matrix constructed from B which has zeros in every row
    for which the corresponding entry for row 'a' in Xa[1,n] is missing.

    Since the matrix whose inverse is required is symmetric positive-definite,
    it's possible to solve this procedure quite efficiently with specialized
    methods.

    Note that this function can accomodate weights and regulatization, but not
    biases. In order to determine the bias for the given row 'a' of X, the B
    matrix should get a column of all-ones appended at the end. Doing this
    also requires passing a matrix 'X' with the bias already-subtracted from
	it when solving for B.

    This function is not meant to exploit multi-threading, but it still calls
    BLAS and LAPACK functions, which set their number of threads externally.

    Parameters
    ----------
    a_vec[k] (out)
        The optimal optimal values for A[1,k].
    k
        The dimensionality of the factorization (a.k.a. latent factors).
    B[n*k]
        The B matrix with which A is multiplied to approximate X.
    n
        Number of rows in B, number of columns in X.
    ldb
        Leading dimension of matrix B (>= k).
    Xa_dense[n]
        Row of X in dense format. Missing values should appear as NAN.
        Will be modified in-place if there are any missing entries. Pass NULL
        if X is sparse.
    full_dense
        Whether the Xa_dense matrix has only non-missing entries.
    Xa[nnz], ixB[nnz], nnz
        Row of the X matrix in sparse format, with ixB denoting the indices
        of the non-missing entries and Xa the values. Pass NULL if X is
        given in dense format.
    weight[nnz or n]
        Observation weights for each non-missing entry in X. Must match the
        shape of X passed - that is, if Xa_dense is passed, must have lenght
        'n', if Xa is passed, must have length 'nnz'. Pass NULL if the weights
        are uniform.
    buffer_FPnum[k^2 or k^2 + n*k]
        Array in which to write temporary values. For sparse X and dense X
        with full_dense=true, must have space for k^2 elements. For dense X
        with full_dense=false, must have space for k^2 + n*k elements.
    lam
        Regularization parameter applied on the A matrix.
    precomputedBtBinvBt
        Precomputed matrix inv(t(B)*B + diag(lam))*t(B). Can only be used
        if passing 'Xa_dense' + 'full_dense=true' + 'weight=NULL'. This is
        used in order to speed up computations, but if not passed, will not be
        used. When passed, there is no requirement for any buffer.
    precomputedBtBw
        Precomputed matrix t(B)*W*B + diag(lam). Will only be used if either:
        (a) passing 'Xa_dense' + 'full_dense=false', and the proportion of
        missing entries in 'Xa_dense' is <= 10% of the total; or
        (b) passing 'Xa_dense=NULL' + 'NA_as_zero=true'.
        This is only used to speed up computations, and if not passed, will
        not be used, unless passing 'NA_as_zero=true'.
    cnt_NA
        Number of missing entries in 'Xa_dense'. Only used if passing
        'precomputedBtBw' and 'full_dense=false'.
    NA_as_zero
        Whether to take missing entries from 'Xa' as zero, and assume that
        there are no missing entries. This implies a different model in which
        the squared error is computed over all values of X. If passing 'true',
        must also pass 'precomputedBtBw'. This is ignored if passing 'Xa_dense'.


*******************************************************************************/
void factors_closed_form
(
    FPnum *restrict a_vec, int k,
    FPnum *restrict B, int n, int ldb,
    FPnum *restrict Xa_dense, bool full_dense,
    FPnum *restrict Xa, int ixB[], size_t nnz,
    FPnum *restrict weight,
    FPnum *restrict buffer_FPnum,
    FPnum lam, FPnum w, FPnum lam_last,
    FPnum *restrict precomputedBtBinvBt,
    FPnum *restrict precomputedBtBw, int cnt_NA, int strideBtB,
    FPnum *restrict precomputedBtBchol, bool NA_as_zero,
    bool use_cg, int max_cg_steps,
    bool force_add_diag
)
{
    FPnum *restrict bufferBtB = buffer_FPnum;
    if (ldb == 0) ldb = k;
    bool add_diag = true;
    char lo = 'L';
    int one = 1;
    int ignore;
    bool prefer_BtB = max2((size_t)cnt_NA, nnz) < (size_t)k;

    if (w != 1.) use_cg = false; /* should not happen, but just in case */


    /* Potential bad inputs */
    if ((Xa_dense != NULL && cnt_NA == n) ||
        (Xa_dense == NULL && nnz == 0))
    {
        set_to_zero(a_vec, k, 1);
        return;
    }

    /* If inv(t(B)*B + diag(lam))*B is already given, use it as a shortcut.
       The intended use-case for this is for cold-start recommendations
       for the collective model with no missing values, given that the
       C matrix is already fixed and is the same for all users. */
    if (precomputedBtBinvBt != NULL && weight == NULL &&
        ((full_dense && Xa_dense != NULL) || (Xa_dense == NULL && NA_as_zero)))
    {
        if (Xa_dense != NULL)
            cblas_tgemv(CblasRowMajor, CblasNoTrans,
                        k, n,
                        1., precomputedBtBinvBt, n,
                        Xa_dense, 1,
                        0., a_vec, 1);
        else
        {
            set_to_zero(a_vec, k, 1);
            tgemv_dense_sp_notrans(
                k, n,
                precomputedBtBinvBt, n,
                ixB, Xa, nnz,
                a_vec
            );
        }
        return;
    }

    /* If t(B*w)*B + diag(lam) is given, and there are very few mising
       values, can still be used as a shortcut by substracting from it */
    else if (Xa_dense != NULL && precomputedBtBw != NULL && weight == NULL &&
             prefer_BtB)
    {
        add_diag = false;
        copy_mat(k, k,
                 precomputedBtBw + strideBtB, k + strideBtB,
                 bufferBtB, k);

        set_to_zero(a_vec, k, 1);
        for (size_t ix = 0; ix < (size_t)n; ix++)
        {
            if (isnan(Xa_dense[ix]))
                cblas_tsyr(CblasRowMajor, CblasUpper, k,
                           -w, B + ix*(size_t)ldb, 1,
                           bufferBtB, k);
            else
                cblas_taxpy(k, Xa_dense[ix], B + ix*(size_t)ldb, 1, a_vec, 1);
        }
        if (w != 1.) cblas_tscal(k, w, a_vec, 1);
    }

    /* If the input is sparse and it's assumed that the non-present
       entries are zero, with no missing values, it's still possible
       to use the precomputed and pre-factorized matrix. */
    else if (NA_as_zero && weight == NULL &&
             Xa_dense == NULL && precomputedBtBchol != NULL)
    {
        set_to_zero(a_vec, k, 1);
        tgemv_dense_sp(n, k,
                       w, B, (size_t)ldb,
                       ixB, Xa, nnz,
                       a_vec);
        tpotrs_(&lo, &k, &one,
                precomputedBtBchol, &k,
                a_vec, &k,
                &ignore);
        return;
    }

    /* In some cases, the sparse matrices might hold zeros instead
       of NAs - here the already-factorized BtBchol should be passed,
       but if for some reason it wasn't, will construct the usual
       matrices on-the-fly.
       This is however slow, and there is no intended use case that
       should end up here.
       If it has weights, could still use the precomputed transpose,
       then adjust by substracting from it again. */
    else if (Xa_dense == NULL && NA_as_zero && !use_cg)
    {
        set_to_zero(a_vec, k, 1);
        if (precomputedBtBw == NULL)
            cblas_tsyrk(CblasRowMajor, CblasUpper, CblasTrans,
                        k, n,
                        w, B, ldb,
                        0., bufferBtB, k);
        else {
            copy_mat(k, k,
                     precomputedBtBw + strideBtB, k + strideBtB,
                     bufferBtB, k);
            add_diag = false;
        }

        if (weight != NULL)
        {
            for (size_t ix = 0; ix < nnz; ix++)
            {
                cblas_tsyr(CblasRowMajor, CblasUpper,
                           k, w * (weight[ix] - 1.),
                           B + (size_t)ixB[ix]*(size_t)ldb, 1,
                           bufferBtB, k);
                cblas_taxpy(k,
                            weight[ix] * Xa[ix],
                            B + (size_t)ixB[ix]*(size_t)ldb, 1,
                            a_vec, 1);
            }
        }

        else {
            tgemv_dense_sp(n, k,
                           1., B, (size_t)ldb,
                           ixB, Xa, nnz,
                           a_vec);
        }

        if (w != 1.) cblas_tscal(k, w, a_vec, 1);
    }

    /* If none of the above apply, it's faster to get an approximate
       solution using the conjugate gradient method.
       In this case, will exit the function afterwards as it will
       not need to calculate the Cholesky. */
    else if (use_cg)
    {
        if (Xa_dense != NULL)
            factors_explicit_cg_dense(
                a_vec, k,
                B, n, ldb,
                Xa_dense, cnt_NA,
                weight,
                precomputedBtBw,
                buffer_FPnum,
                lam, lam_last,
                max_cg_steps
            );
        else if (NA_as_zero && weight != NULL)
            factors_explicit_cg_NA_as_zero_weighted(
                a_vec, k,
                B, n, ldb,
                Xa, ixB, nnz,
                weight,
                precomputedBtBw,
                buffer_FPnum,
                lam, lam_last,
                max_cg_steps
            );
        else
            factors_explicit_cg(
                a_vec, k,
                B, n, ldb,
                Xa, ixB, nnz,
                weight,
                buffer_FPnum,
                lam, lam_last,
                max_cg_steps
            );

        return;
    }

    /* In more general cases, need to construct the following matrices:
        - t(B)*B + diag(lam), with only the rows of B which have a
          non-missing entry in X.
        - t(B)*t(X), with missing entries in X set to zero.
       If X is dense, this can be accomplished by following the steps verbatim.
       If X is sparse, it's more efficient to obtain the first one through
       SR1 updates to an empty matrix, while the second one can be obtained
       with a dense-sparse matrix multiplication */

    else if (Xa_dense == NULL)
    {
        /* Sparse X - obtain t(B)*B through SR1 updates, avoiding a full
           matrix-matrix multiplication. This is the expected scenario for
           most use-cases. */
        set_to_zero(bufferBtB, square(k), 1);
        for (size_t ix = 0; ix < nnz; ix++)
            cblas_tsyr(CblasRowMajor, CblasUpper, k,
                       (weight == NULL)? (1.) : (weight[ix]),
                       B + (size_t)ixB[ix]*(size_t)ldb, 1,
                       bufferBtB, k);

        /* Now obtain t(B)*t(X) from a dense_matrix-sparse_vector product,
           avoid again a full matrix-vector multiply. Note that this is
           stored in 'a_vec', despite the name */
        set_to_zero(a_vec, k, 1);
        if (weight == NULL) {
            tgemv_dense_sp(n, k,
                           1., B, (size_t)ldb,
                           ixB, Xa, nnz,
                           a_vec);
        }

        else {
            tgemv_dense_sp_weighted(n, k,
                                    weight, B, (size_t)ldb,
                                    ixB, Xa, nnz,
                                    a_vec);
        }

        if (w != 1.) {
            cblas_tscal(k, w, a_vec, 1);
            cblas_tscal(square(k), w, bufferBtB, 1);
        }
    }

    else
    {
        /* Dense X - in this case, re-construct t(B)*B without the missing
           entries - at once if possible, or one-by-one if not.
           Note that, if B0 is the B matrix with the rows set to zero
           when the entries of X are missing, then the following
           equalities will apply:
            t(B0)*B == t(B)*B0 == t(B0)*B0 */
        if (full_dense && weight == NULL)
        {
            /* this will only be encountered when calculating factors
               after having called 'fit_*'. */
            cblas_tsyrk(CblasRowMajor, CblasUpper, CblasTrans,
                        k, n,
                        w, B, ldb,
                        0., bufferBtB, k);
            cblas_tgemv(CblasRowMajor, CblasTrans,
                        n, k,
                        w, B, ldb,
                        Xa_dense, 1,
                        0., a_vec, 1);
        }

        else
        {
            set_to_zero(a_vec, k, 1);
            set_to_zero(bufferBtB, square(k), 1);
            for (size_t ix = 0; ix < (size_t)n; ix++) {
                if (!isnan(Xa_dense[ix])) {
                    cblas_tsyr(CblasRowMajor, CblasUpper,
                               k, (weight == NULL)? (1.) : (weight[ix]),
                               B + ix*(size_t)ldb, 1,
                               bufferBtB, k);
                    cblas_taxpy(k,
                                ((weight == NULL)? (1.) : (weight[ix]))
                                    * Xa_dense[ix],
                                B + ix*(size_t)ldb, 1,
                                a_vec, 1);
                }
            }
            if (w != 1.) {
                cblas_tscal(k, w, a_vec, 1);
                cblas_tscal(square(k), w, bufferBtB, 1);
            }
        }
    }

    /* Finally, obtain closed-form through Cholesky factorization,
       exploiting the fact that t(B)*B+diag(lam) is symmetric PSD */
    if (add_diag || force_add_diag) {
        add_to_diag(bufferBtB, lam, k);
        if (lam_last != lam) bufferBtB[square(k)-1] += (lam_last - lam);
    }

    tposv_(&lo, &k, &one,
           bufferBtB, &k,
           a_vec, &k,
           &ignore);
    /* Note: Function 'posv' is taken from LAPACK for FORTRAN.
       If using LAPACKE for C with with Row-Major parameter,
       some implementations will copy the matrix to transpose it
       and pass it to FORTRAN-posv. */
}

/* https://en.wikipedia.org/wiki/Conjugate_gradient_method */
void factors_explicit_cg
(
    FPnum *restrict a_vec, int k,
    FPnum *restrict B, int n, int ldb,
    FPnum *restrict Xa, int ixB[], size_t nnz,
    FPnum *restrict weight,
    FPnum *restrict buffer_FPnum,
    FPnum lam, FPnum lam_last,
    int max_cg_steps
)
{
    FPnum *restrict Ap = buffer_FPnum;
    FPnum *restrict p  = Ap + k;
    FPnum *restrict r  = p  + k;
    set_to_zero(r, k, 1);
    FPnum coef;
    FPnum a;
    FPnum r_old, r_new;

    if (weight == NULL)
        tgemv_dense_sp(n, k,
                       1., B, (size_t)ldb,
                       ixB, Xa, nnz,
                       r);
    else
        tgemv_dense_sp_weighted(n, k,
                                weight, B, (size_t)ldb,
                                ixB, Xa, nnz,
                                r);

    for (size_t ix = 0; ix < nnz; ix++) {
        coef = cblas_tdot(k, B + (size_t)ixB[ix]*(size_t)ldb, 1, a_vec, 1);
        coef *= (weight == NULL)? 1. : weight[ix];
        cblas_taxpy(k, -coef, B + (size_t)ixB[ix]*ldb, 1, r, 1);
    }
    cblas_taxpy(k, -lam, a_vec, 1, r, 1);
    if (lam != lam_last)
        r[k-1] -= (lam_last-lam) * a_vec[k-1];

    copy_arr(r, p, k, 1);
    r_old = cblas_tdot(k, r, 1, r, 1);

    #ifdef FORCE_CG
    if (r_old <= 1e-15)
        return;
    #else
    if (r_old <= 1e-12)
        return;
    #endif

    for (int cg_step = 0; cg_step < max_cg_steps; cg_step++)
    {
        set_to_zero(Ap, k, 1);
        for (size_t ix = 0; ix < nnz; ix++) {
            coef = cblas_tdot(k, B + (size_t)ixB[ix]*(size_t)ldb, 1, p, 1);
            coef *= (weight == NULL)? 1. : weight[ix];
            cblas_taxpy(k, coef, B + (size_t)ixB[ix]*ldb, 1, Ap, 1);
        }
        cblas_taxpy(k, lam, p, 1, Ap, 1);
        if (lam != lam_last)
            Ap[k-1] += (lam_last-lam) * p[k-1];

        a = r_old / cblas_tdot(k, p, 1, Ap, 1);
        cblas_taxpy(k,  a,  p, 1, a_vec, 1);
        cblas_taxpy(k, -a, Ap, 1, r, 1);

        r_new = cblas_tdot(k, r, 1, r, 1);
        #ifdef FORCE_CG
        if (r_new <= 1e-15)
            break;
        #else
        if (r_new <= 1e-8)
            break;
        #endif

        cblas_tscal(k, r_new / r_old, p, 1);
        cblas_taxpy(k, 1., r, 1, p, 1);
        r_old = r_new;
    }
}

void factors_explicit_cg_NA_as_zero_weighted
(
    FPnum *restrict a_vec, int k,
    FPnum *restrict B, int n, int ldb,
    FPnum *restrict Xa, int ixB[], size_t nnz,
    FPnum *restrict weight,
    FPnum *restrict precomputedBtBw, /* should NOT be multiplied by 'w' */
    FPnum *restrict buffer_FPnum,
    FPnum lam, FPnum lam_last,
    int max_cg_steps
)
{
    FPnum *restrict Ap = buffer_FPnum;
    FPnum *restrict p  = Ap + k;
    FPnum *restrict r  = p  + k;
    FPnum *restrict wr = r  + k; /* length is 'n' */
    set_to_zero(r, k, 1);
    FPnum a;
    FPnum r_old, r_new, coef;

    bool prefer_BtB = nnz < (size_t)(2*k);

    tgemv_dense_sp_weighted(n, k,
                            weight, B, (size_t)ldb,
                            ixB, Xa, nnz,
                            r);
    if (precomputedBtBw != NULL && prefer_BtB)
    {
        cblas_tsymv(CblasRowMajor, CblasUpper, k,
                    -1., precomputedBtBw, k,
                    a_vec, 1,
                    1., r, 1);
        for (size_t ix = 0; ix < nnz; ix++)
        {
            coef = cblas_tdot(k,
                              B + (size_t)ixB[ix]*(size_t)ldb, 1,
                              a_vec, 1);
            cblas_taxpy(k, -((weight[ix]-1.) * coef),
                        B + ix*(size_t)ldb, 1,
                        r, 1);
        }
    }

    else
    {
        cblas_tgemv(CblasRowMajor, CblasNoTrans,
                    n, k,
                    -1., B, ldb,
                    a_vec, 1,
                    0., wr, 1);
        for (size_t ix = 0; ix < nnz; ix++)
            wr[ixB[ix]] *= weight[ix];
        cblas_tgemv(CblasRowMajor, CblasTrans,
                    n, k,
                    1., B, ldb,
                    wr, 1,
                    1., r, 1);
    }


    cblas_taxpy(k, -lam, a_vec, 1, r, 1);
    if (lam != lam_last)
        r[k-1] -= (lam_last-lam) * a_vec[k-1];

    copy_arr(r, p, k, 1);
    r_old = cblas_tdot(k, r, 1, r, 1);

    #ifdef FORCE_CG
    if (r_old <= 1e-15)
        return;
    #else
    if (r_old <= 1e-12)
        return;
    #endif

    for (int cg_step = 0; cg_step < max_cg_steps; cg_step++)
    {
        if (precomputedBtBw != NULL && prefer_BtB)
        {
            cblas_tsymv(CblasRowMajor, CblasUpper, k,
                        1., precomputedBtBw, k,
                        p, 1,
                        0., Ap, 1);
            for (size_t ix = 0; ix < nnz; ix++) {
                coef = cblas_tdot(k,
                                  B + (size_t)ixB[ix]*(size_t)ldb, 1,
                                  p, 1);
                cblas_taxpy(k, (weight[ix] - 1.) * coef,
                            B + (size_t)ixB[ix]*(size_t)ldb, 1,
                            Ap, 1);
            }
        }

        else
        {
            cblas_tgemv(CblasRowMajor, CblasNoTrans,
                        n, k,
                        1., B, ldb,
                        p, 1,
                        0., wr, 1);
            for (size_t ix = 0; ix < nnz; ix++)
                wr[ixB[ix]] *= weight[ix];
            cblas_tgemv(CblasRowMajor, CblasTrans,
                        n, k,
                        1., B, ldb,
                        wr, 1,
                        0., Ap, 1);
        }

        cblas_taxpy(k, lam, p, 1, Ap, 1);
        if (lam != lam_last)
            Ap[k-1] += (lam_last-lam) * p[k-1];

        a = r_old / cblas_tdot(k, p, 1, Ap, 1);
        cblas_taxpy(k,  a,  p, 1, a_vec, 1);
        cblas_taxpy(k, -a, Ap, 1, r, 1);

        r_new = cblas_tdot(k, r, 1, r, 1);
        #ifdef FORCE_CG
        if (r_new <= 1e-15)
            break;
        #else
        if (r_new <= 1e-8)
            break;
        #endif

        cblas_tscal(k, r_new / r_old, p, 1);
        cblas_taxpy(k, 1., r, 1, p, 1);
        r_old = r_new;
    }
}

void factors_explicit_cg_dense
(
    FPnum *restrict a_vec, int k,
    FPnum *restrict B, int n, int ldb,
    FPnum *restrict Xa_dense, int cnt_NA,
    FPnum *restrict weight,
    FPnum *restrict precomputedBtBw, /* should NOT be multiplied by 'w' */
    FPnum *restrict buffer_FPnum,
    FPnum lam, FPnum lam_last,
    int max_cg_steps
)
{
    FPnum *restrict Ap = buffer_FPnum;
    FPnum *restrict p  = Ap + k;
    FPnum *restrict r  = p  + k;
    FPnum r_new, r_old;
    FPnum a, coef, w_this;

    bool prefer_BtB = cnt_NA < k && precomputedBtBw != NULL && weight == NULL;
    if (!prefer_BtB)
        set_to_zero(r, k, 1);

    if (prefer_BtB)
    {
        cblas_tsymv(CblasRowMajor, CblasUpper, k,
                    -1., precomputedBtBw, k,
                    a_vec, 1,
                    0., r, 1);
        for (size_t ix = 0; ix < (size_t)n; ix++)
        {
            if (isnan(Xa_dense[ix])) {
                coef = cblas_tdot(k, B + ix*(size_t)ldb, 1, a_vec, 1);
                cblas_taxpy(k, coef, B + ix*(size_t)ldb, 1, r, 1);
            }
            
            else {
                cblas_taxpy(k, Xa_dense[ix], B + ix*(size_t)ldb, 1, r, 1);
            }
        }
    }

    else
    {
        for (size_t ix = 0; ix < (size_t)n; ix++)
        {
            if (!isnan(Xa_dense[ix]))
            {
                w_this = (weight == NULL)? 1. : weight[ix];
                cblas_taxpy(k,
                            w_this * Xa_dense[ix], B + ix*(size_t)ldb, 1,
                            r, 1);
                coef = cblas_tdot(k, B + ix*(size_t)ldb, 1, a_vec, 1);
                cblas_taxpy(k, -w_this * coef, B + ix*(size_t)ldb, 1, r, 1);
            }
        }
    }
    
    cblas_taxpy(k, -lam, a_vec, 1, r, 1);
    if (lam != lam_last)
        r[k-1] -= (lam_last-lam) * a_vec[k-1];

    copy_arr(r, p, k, 1);
    r_old = cblas_tdot(k, r, 1, r, 1);

    #ifdef FORCE_CG
    if (r_old <= 1e-15)
        return;
    #else
    if (r_old <= 1e-12)
        return;
    #endif

    for (int cg_step = 0; cg_step < max_cg_steps; cg_step++)
    {
        if (prefer_BtB)
        {
            cblas_tsymv(CblasRowMajor, CblasUpper, k,
                        1., precomputedBtBw, k,
                        p, 1,
                        0., Ap, 1);
            for (size_t ix = 0; ix < (size_t)n; ix++)
                if (isnan(Xa_dense[ix])) {
                    coef = cblas_tdot(k,  B + ix*(size_t)ldb, 1, p,  1);
                    cblas_taxpy(k, -coef, B + ix*(size_t)ldb, 1, Ap, 1);
                }
        }

        else
        {
            set_to_zero(Ap, k, 1);
            for (size_t ix = 0; ix < (size_t)n; ix++)
            {
                if (!isnan(Xa_dense[ix]))
                {
                    w_this = (weight == NULL)? 1. : weight[ix];
                    coef = cblas_tdot(k, B + ix*(size_t)ldb, 1, p, 1);
                    cblas_taxpy(k, w_this * coef, B + ix*(size_t)ldb, 1, Ap, 1);
                }
            }
        }

        cblas_taxpy(k, lam, p, 1, Ap, 1);
        if (lam != lam_last)
            Ap[k-1] += (lam_last-lam) * p[k-1];

        a = r_old / cblas_tdot(k, p, 1, Ap, 1);
        cblas_taxpy(k,  a,  p, 1, a_vec, 1);
        cblas_taxpy(k, -a, Ap, 1, r, 1);
        r_new = cblas_tdot(k, r, 1, r, 1);
        #ifdef FORCE_CG
        if (r_new <= 1e-15)
            break;
        #else
        if (r_new <= 1e-8)
            break;
        #endif

        cblas_tscal(k, r_new / r_old, p, 1);
        cblas_taxpy(k, 1., r, 1, p, 1);
        r_old = r_new;
    }
}


/* https://www.benfrederickson.com/fast-implicit-matrix-factorization/ */
void factors_implicit_cg
(
    FPnum *restrict a_vec, int k,
    FPnum *restrict B, size_t ldb,
    FPnum *restrict Xa, int ixB[], size_t nnz,
    FPnum lam,
    FPnum *restrict precomputedBtBw, int strideBtB,
    int max_cg_steps,
    FPnum *restrict buffer_FPnum,
    bool force_add_diag
)
{
    FPnum *restrict Ap = buffer_FPnum;
    FPnum *restrict r  = Ap + k;
    FPnum *restrict p  = r  + k;
    FPnum coef;
    FPnum r_old, r_new;
    FPnum a;

    if (force_add_diag) {
        /* Note: there's no intended use-case that would end up here */
        copy_arr(precomputedBtBw, buffer_FPnum, square(k), 1);
        precomputedBtBw = buffer_FPnum;
        add_to_diag(precomputedBtBw, lam, k);
        buffer_FPnum += square((size_t)k);
    }

    cblas_tsymv(CblasRowMajor, CblasUpper, k,
                -1., precomputedBtBw, k,
                a_vec, 1,
                0., r, 1);
    for (size_t ix = 0; ix < nnz; ix++) {
        coef = cblas_tdot(k, B + (size_t)ixB[ix]*ldb, 1, a_vec, 1);
        cblas_taxpy(k,
                    -(coef - 1.) * Xa[ix] - coef,
                    B + (size_t)ixB[ix]*ldb, 1,
                    r, 1);
    }

    copy_arr(r, p, k, 1);
    r_old = cblas_tdot(k, r, 1, r, 1);

    #ifdef FORCE_CG
    if (r_old <= 1e-15)
        return;
    #else
    if (r_old <= 1e-12)
        return;
    #endif

    for (int cg_step = 0; cg_step < max_cg_steps; cg_step++)
    {
        cblas_tsymv(CblasRowMajor, CblasUpper, k,
                    1., precomputedBtBw, k,
                    p, 1,
                    0., Ap, 1);
        for (size_t ix = 0; ix < nnz; ix++) {
            coef = cblas_tdot(k, B + (size_t)ixB[ix]*ldb, 1, p, 1);
            cblas_taxpy(k,
                        (coef - 1.) * Xa[ix] + coef,
                        B + (size_t)ixB[ix]*ldb, 1,
                        Ap, 1);
        }

        a = r_old / cblas_tdot(k, Ap, 1, p, 1);
        cblas_taxpy(k,  a,  p, 1, a_vec, 1);
        cblas_taxpy(k, -a, Ap, 1, r, 1);
        r_new = cblas_tdot(k, r, 1, r, 1);
        #ifdef FORCE_CG
        if (r_new <= 1e-15)
            break;
        #else
        if (r_new <= 1e-8)
            break;
        #endif
        cblas_tscal(k, r_new / r_old, p, 1);
        cblas_taxpy(k, 1., r, 1, p, 1);
        r_old = r_new;
    }
}


void factors_implicit_chol
(
    FPnum *restrict a_vec, int k,
    FPnum *restrict B, size_t ldb,
    FPnum *restrict Xa, int ixB[], size_t nnz,
    FPnum lam,
    FPnum *restrict precomputedBtBw, int strideBtB,
    bool zero_out,
    FPnum *restrict buffer_FPnum,
    bool force_add_diag
)
{
    char uplo = 'L';
    int one = 1;
    int ignore;
    FPnum *restrict BtBw = buffer_FPnum;
    if (strideBtB == 0)
        memcpy(BtBw, precomputedBtBw, (size_t)square(k)*sizeof(FPnum));
    else
        copy_mat(k, k,
                 precomputedBtBw + strideBtB + strideBtB*k, k + strideBtB,
                 BtBw, k);
    if (zero_out) set_to_zero(a_vec, k, 1);

    for (size_t ix = 0; ix < nnz; ix++) {
        cblas_tsyr(CblasRowMajor, CblasUpper, k,
                   Xa[ix], B + (size_t)ixB[ix]*ldb, 1,
                   BtBw, k);
        cblas_taxpy(k, Xa[ix] + 1.,
                    B + (size_t)ixB[ix]*ldb, 1, a_vec, 1);
    }

    if (force_add_diag)
        add_to_diag(BtBw, lam, k);

    tposv_(&uplo, &k, &one,
           BtBw, &k,
           a_vec, &k,
           &ignore);
}

void factors_implicit
(
    FPnum *restrict a_vec, int k,
    FPnum *restrict B, size_t ldb,
    FPnum *restrict Xa, int ixB[], size_t nnz,
    FPnum lam,
    FPnum *restrict precomputedBtBw, int strideBtB,
    bool zero_out, bool use_cg, int max_cg_steps,
    FPnum *restrict buffer_FPnum,
    bool force_add_diag
)
{
    if (nnz == 0)
        return set_to_zero(a_vec, k, 1);

    if (use_cg)
        factors_implicit_cg(
            a_vec, k,
            B, ldb,
            Xa, ixB, nnz,
            lam,
            precomputedBtBw, strideBtB,
            max_cg_steps,
            buffer_FPnum,
            force_add_diag
        );
    else
        factors_implicit_chol(
            a_vec, k,
            B, ldb,
            Xa, ixB, nnz,
            lam,
            precomputedBtBw, strideBtB,
            zero_out,
            buffer_FPnum,
            force_add_diag
        );
}

FPnum fun_grad_Adense
(
    FPnum *restrict g_A,
    FPnum *restrict A, int lda,
    FPnum *restrict B, int ldb,
    int m, int n, int k,
    FPnum *restrict Xfull, FPnum *restrict weight,
    FPnum lam, FPnum w, FPnum lam_last,
    bool do_B, bool reset_grad,
    int nthreads,
    FPnum *restrict buffer_FPnum
)
{
    #if defined(_OPENMP) && \
                ( (_OPENMP < 200801)  /* OpenMP < 3.0 */ \
                  || defined(_WIN32) || defined(_WIN64) \
                )
    long long ix;
    #endif
    FPnum *g_B = NULL;
    if (do_B) g_B = g_A;
    FPnum f = 0.;
    size_t m_by_n = (size_t)m * (size_t)n;

    copy_arr(Xfull, buffer_FPnum, m_by_n, nthreads);
    cblas_tgemm(CblasRowMajor, CblasNoTrans, CblasTrans,
                m, n, k,
                1., A, lda, B, ldb,
                -1., buffer_FPnum, n);
    if (weight == NULL) {
        nan_to_zero(buffer_FPnum, Xfull, m_by_n, nthreads);
        f = w * sum_squares(buffer_FPnum, m_by_n, nthreads);
    }
    else {
        /* TODO: make it compensated summation */
        #pragma omp parallel for schedule(static) num_threads(nthreads) \
                shared(buffer_FPnum, m, n, weight) reduction(+:f)
        for (size_t_for ix = 0; ix < m_by_n; ix++)
            f += (!isnan(buffer_FPnum[ix]))?
                  (square(buffer_FPnum[ix]) * w*weight[ix]) : (0);
        mult_if_non_nan(buffer_FPnum, Xfull, weight, m_by_n, nthreads);
    }
    if (!do_B)
        cblas_tgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                    m, k, n,
                    w, buffer_FPnum, n, B, ldb,
                    reset_grad? 0. : 1., g_A, lda);
    else
        cblas_tgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                    n, k, m,
                    w, buffer_FPnum, n, A, lda,
                    reset_grad? 0. : 1., g_B, ldb);
    if (lam != 0)
    {
        if (!do_B)
            add_lam_to_grad_and_fun(&f, g_A, A, m, k, lda, lam, nthreads);
        else
            add_lam_to_grad_and_fun(&f, g_B, B, n, k, ldb, lam, nthreads);
    }
    if (lam != 0. && lam_last != lam && k >= 1) {
        if (!do_B) {
            cblas_taxpy(m, lam_last-lam, A + k-1, lda, g_A + k-1, lda);
            f += (lam_last-lam) * cblas_tdot(m, A + k-1, lda, A + k-1, lda);
        }
        else {
            cblas_taxpy(n, lam_last-lam, B + k-1, ldb, g_B + k-1, ldb);
            f += (lam_last-lam) * cblas_tdot(n, B + k-1, ldb, B + k-1, ldb);
        }
    }
    return f / 2.;
}

void add_lam_to_grad_and_fun
(
    FPnum *restrict fun,
    FPnum *restrict grad,
    FPnum *restrict A,
    int m, int k, int lda,
    FPnum lam, int nthreads
)
{
    #if defined(_OPENMP) && \
                ( (_OPENMP < 200801)  /* OpenMP < 3.0 */ \
                  || defined(_WIN32) || defined(_WIN64) \
                )
    long long row;
    #endif
    if (lda == k)
    {
        taxpy_large(A, lam, grad, (size_t)m*(size_t)k, nthreads);
        *fun += lam * sum_squares(A, (size_t)m*(size_t)k, nthreads);
    }

    else
    {
        #pragma omp parallel for schedule(static) num_threads(nthreads) \
                shared(m, k, A, grad, lam, lda)
        for (size_t_for row = 0; row < (size_t)m; row++)
            for (size_t col = 0; col < (size_t)k; col++)
                grad[col + row*lda] += lam * A[col + row*lda];
        FPnum reg = 0;
        #pragma omp parallel for schedule(static) num_threads(nthreads) \
                shared(m, k, A, lda) reduction(+:reg)
        for (size_t_for row = 0; row < (size_t)m; row++)
            for (size_t col = 0; col < (size_t)k; col++)
                reg += square(A[col + row*lda]);
        *fun += lam * reg;
    }
}

FPnum wrapper_fun_grad_Adense
(
    void *instance,
    lbfgsFPnumval_t *x,
    lbfgsFPnumval_t *g,
    const size_t n,
    const lbfgsFPnumval_t step
)
{
    data_fun_grad_Adense *data = (data_fun_grad_Adense*)instance;
    return  fun_grad_Adense(
                g,
                x, data->lda,
                data->B, data->ldb,
                data->m, data->n, data->k,
                data->Xfull, data->weight,
                data->lam, data->w, data->lam_last,
                false, true,
                data->nthreads,
                data->buffer_FPnum
            );
}

FPnum wrapper_fun_grad_Bdense
(
    void *instance,
    lbfgsFPnumval_t *x,
    lbfgsFPnumval_t *g,
    const size_t n,
    const lbfgsFPnumval_t step
)
{
    data_fun_grad_Bdense *data = (data_fun_grad_Bdense*)instance;
    return  fun_grad_Adense(
                g,
                data->A, data->lda,
                x, data->ldb,
                data->m, data->n, data->k,
                data->Xfull, data->weight,
                data->lam, data->w, data->lam_last,
                true, true,
                data->nthreads,
                data->buffer_FPnum
            );
}

void buffer_size_optimizeA
(
    size_t *buffer_size, size_t *buffer_lbfgs_size,
    int m, int n, int k, int lda, int nthreads,
    bool do_B, bool NA_as_zero,
    bool use_cg, bool finalize_chol,
    bool full_dense, bool near_dense,
    bool has_dense, bool has_weight
)
{
    if (finalize_chol)
    {
        size_t buffer_size_cg = 0;
        size_t buffer_size_chol = 0;
        size_t buffer_lbfgs_size_cg = 0;
        size_t buffer_lbfgs_size_chol = 0;
        buffer_size_optimizeA(
            &buffer_size_chol, &buffer_lbfgs_size_chol,
            m, n, k, lda, nthreads,
            do_B, NA_as_zero,
            true, false,
            full_dense, near_dense,
            has_dense, has_weight
        );
        buffer_size_optimizeA(
            &buffer_size_cg, &buffer_lbfgs_size_cg,
            m, n, k, lda, nthreads,
            do_B, NA_as_zero,
            false, false,
            full_dense, near_dense,
            has_dense, has_weight
        );

        *buffer_size = max2(buffer_size_cg, buffer_size_chol);
        *buffer_lbfgs_size = max2(buffer_lbfgs_size_cg, buffer_lbfgs_size_chol);
        return;
    }

    if (has_dense && (full_dense || near_dense) && !has_weight)
    {
        *buffer_size = (size_t)2 * (size_t)square(k);
        if (do_B) *buffer_size += (size_t)nthreads * (size_t)n;
        if (!full_dense)
        {
            *buffer_size += (size_t)nthreads * max2(
                                (size_t)square(k),
                                (size_t)(use_cg? (3 * k) : 0)
                            );

        }
    }

    else if (has_dense)
    {
        *buffer_lbfgs_size = 4;
        *buffer_size = (size_t)m * (size_t)n;
        *buffer_size += (size_t)13*((size_t)m * (size_t)lda - (size_t)(lda-k));
    }

    else if (!has_dense && NA_as_zero && !has_weight)
    {
        *buffer_size = square(k);
    }

    else
    {
        *buffer_size = square(k);
        if (use_cg)
            *buffer_size = max2(*buffer_size, (size_t)(3 * k));
        if (use_cg && !has_dense)
            *buffer_size = (size_t)(3 * k + (NA_as_zero? n : 0));
        *buffer_size *= (size_t)nthreads;

        if (!has_dense && NA_as_zero && !use_cg)
            *buffer_size += square(k);

    }
}

void buffer_size_optimizeA_implicit
(
    size_t *buffer_size,
    int k, int nthreads,
    bool use_precomputed,
    bool use_cg, bool finalize_chol
)
{
    if (finalize_chol)
    {
        size_t buffer_size_chol = 0;
        size_t buffer_size_cg = 0;
        buffer_size_optimizeA_implicit(
            &buffer_size_chol,
            k, nthreads,
            use_precomputed,
            false, false
        );
        buffer_size_optimizeA_implicit(
            &buffer_size_cg,
            k, nthreads,
            use_precomputed,
            true, false
        );
        *buffer_size = max2(buffer_size_chol, buffer_size_cg);
        return;
    }

    size_t size_thread_buffer = use_cg? (3 * k) : (square(k));
    *buffer_size = (size_t)(use_precomputed? 0 : square(k))
                    + (size_t)nthreads * size_thread_buffer;
}

void optimizeA
(
    FPnum *restrict A, int lda,
    FPnum *restrict B, int ldb,
    int m, int n, int k,
    size_t Xcsr_p[], int Xcsr_i[], FPnum *restrict Xcsr,
    FPnum *restrict Xfull, bool full_dense, bool near_dense,
    int cnt_NA[], FPnum *restrict weight, bool NA_as_zero,
    FPnum lam, FPnum w, FPnum lam_last,
    bool do_B, bool is_first_iter,
    int nthreads,
    bool use_cg, int max_cg_steps,
    FPnum *restrict buffer_FPnum,
    iteration_data_t *buffer_lbfgs_iter
)
{
    /* TODO: handle case of all-missing when the values are not reset to zero */
    #if defined(_OPENMP) && \
                ( (_OPENMP < 200801)  /* OpenMP < 3.0 */ \
                  || defined(_WIN32) || defined(_WIN64) \
                )
    long long ix;
    #endif
    char uplo = 'L';
    int ignore;
    if (w != 1.) {
        lam /= w; /* 'w' and 'lam' only matter relative to each other */
        lam_last /= w;
    }

    /* Case 1: X is full dense with few or no missing values.
       Here can apply the closed-form solution with only
       one multiplication by B for all rows at once.
       If there is a small amount of observations with missing
       values, can do a post-hoc pass over them to obtain their
       solutions individually. */
    if (Xfull != NULL && (full_dense || near_dense) && weight == NULL)
    {
        FPnum *restrict bufferBtB = buffer_FPnum;
        FPnum *restrict bufferBtBcopy = buffer_FPnum + square(k);
        FPnum *restrict bufferX = bufferBtBcopy + square(k);
        FPnum *restrict buffer_remainder = bufferX + (do_B? (n*nthreads) : (0));
        /* t(B)*B + diag(lam) */
        cblas_tsyrk(CblasRowMajor, CblasUpper, CblasTrans,
                    k, n,
                    1., B, ldb,
                    0., bufferBtB, k);
        add_to_diag(bufferBtB, lam, k);
        if (lam_last != lam) bufferBtB[square(k)-1] += (lam_last - lam);
        if (near_dense) /* Here will also need t(B)*B alone */
            memcpy(bufferBtBcopy, bufferBtB, (size_t)square(k)*sizeof(FPnum));
        /* t(B)*t(X)
           Note: this will be passed to LAPACK function which assumes
           column-major order, thus must pass the transpose.
           Note2: if passing 'do_B=true', the inputs will all be
           swapped, so what here says 'A' will be 'B', what says
           'm' will be 'n', and so on. But the matrix 'X' remains
           the same, so the inputs need to be transposed for B. */
        if (!do_B) 
            cblas_tgemm(CblasRowMajor, CblasNoTrans, CblasNoTrans,
                        m, k, n,
                        1., Xfull, n, B, ldb,
                        0., A, lda);
        else
            cblas_tgemm(CblasRowMajor, CblasTrans, CblasNoTrans,
                        m, k, n,
                        1., Xfull, m, B, ldb,
                        0., A, lda);

        
        #ifdef FORCE_NO_NAN_PROPAGATION
        if (!full_dense)
            #pragma omp parallel for schedule(static) \
                    num_threads(min2(4, nthreads)) \
                    shared(A, m, lda)
            for (size_t_for ix = 0;
                 ix < (size_t)m*(size_t)lda - (size_t)(lda-k);
                 ix++)
                A[ix] = isnan(A[ix])? 0 : A[ix];
        #endif
        /* A = t( inv(t(B)*B + diag(lam)) * t(B)*t(X) )
           Note: don't try to flip the equation as the 'posv'
           function assumes only the LHS is symmetric. */
        tposv_(&uplo, &k, &m,
               bufferBtB, &k,
               A, &lda,
               &ignore);
        /* If there are some few rows with missing values, now do a
           post-hoc pass over them only */
        if (near_dense)
        {
            size_t size_buffer = square(k);
            if (use_cg)
                size_buffer = max2(size_buffer, (size_t)(3 * k));

            #pragma omp parallel for schedule(dynamic) num_threads(nthreads) \
                    shared(A, lda, B, ldb, m, n, k, lam, lam_last, weight, \
                           cnt_NA, Xfull, buffer_remainder, bufferBtBcopy, \
                           nthreads, use_cg) \
                    firstprivate(bufferX)
            for (size_t_for ix = 0; ix < (size_t)m; ix++)
                if (cnt_NA[ix] > 0) {
                    if (!do_B)
                        bufferX = Xfull + ix*(size_t)n;
                    else
                        cblas_tcopy(n, Xfull + ix, m,
                                    bufferX
                            + ((size_t)n*(size_t)omp_get_thread_num()), 1);

                    if (use_cg)
                        set_to_zero(A + ix*(size_t)lda, k, 1);

                    factors_closed_form(
                        A + ix*(size_t)lda, k,
                        B, n, ldb,
                        bufferX + (do_B?
                                    ((size_t)n*(size_t)omp_get_thread_num())
                                        :
                                    ((size_t)0)), false,
                        (FPnum*)NULL, (int*)NULL, (size_t)0,
                        (FPnum*)NULL,
                        buffer_remainder
                         + size_buffer * (size_t)omp_get_thread_num(),
                        lam, 1., lam_last,
                        (FPnum*)NULL,
                        bufferBtBcopy, cnt_NA[ix], 0,
                        (FPnum*)NULL, false,
                        use_cg, k, /* <- A was reset to zero, need more steps */
                        false
                    );
                }
        }
    }

    /* TODO: avoid using L-BFGS when using 'use_cg' */

    /* Case 2: X is dense, but has many missing values or has weights.
       Here will do them all individually, pre-calculating only
         t(B)*B + diag(lam)
       in case some rows have few missing values. */
    else if (Xfull != NULL)
    {
        // FPnum *restrict bufferBtB = buffer_FPnum;
        // FPnum *restrict bufferX = bufferBtB + square(k);
        // FPnum *restrict bufferW = bufferX + (do_B? (n*nthreads) : (0));
        // FPnum *restrict buffer_remainder = bufferW + ((do_B && weight != NULL)?
        //                                               (n*nthreads) : (0));

        // if (weight == NULL)
        //     bufferW = NULL;

        // cblas_tsyrk(CblasRowMajor, CblasUpper, CblasTrans,
        //             k, n,
        //             1., B, ldb,
        //             0., bufferBtB, k);
        // add_to_diag(bufferBtB, lam, k);
        // if (lam_last != lam) bufferBtB[square(k)-1] += (lam_last - lam);

        // #pragma omp parallel for schedule(dynamic) num_threads(nthreads) \
        //         shared(Xfull, weight, do_B, m, n, k, A, lda, B, ldb, \
        //                lam, lam_last, bufferBtB, cnt_NA, buffer_remainder, \
        //                use_cg, max_cg_steps) \
        //         firstprivate(bufferX, bufferW)
        // for (size_t_for ix = 0; ix < (size_t)m; ix++)
        // {
        //     if (!do_B) {
        //         bufferX = Xfull + ix*(size_t)n;
        //         if (weight != NULL)
        //             bufferW = weight + ix*(size_t)n;
        //     }
        //     else {
        //         cblas_tcopy(n, Xfull + ix, m,
        //                     bufferX + ((size_t)n*(size_t)omp_get_thread_num()), 1);
        //         if (weight != NULL)
        //             cblas_tcopy(n, weight + ix, m,
        //                         bufferW + ((size_t)n*(size_t)omp_get_thread_num()), 1);
        //     }

        //     /* TODO: revise the size of the thread-local space */
        //     factors_closed_form(
        //         A + ix*(size_t)lda, k,
        //         B, n, ldb,
        //         bufferX + (do_B? ((size_t)n*(size_t)omp_get_thread_num()) : ((size_t)0)), cnt_NA[ix]==0,
        //         (FPnum*)NULL, (int*)NULL, (size_t)0,
        //         (weight != NULL)?
        //             (bufferW + (do_B? ((size_t)n*(size_t)omp_get_thread_num()) : ((size_t)0)))
        //               :
        //             ((FPnum*)NULL),
        //         buffer_remainder
        //          + (((size_t)n*2 + 3*(size_t)square(k)
        //               + (use_cg? (size_t)6*(size_t)k : (size_t)0))
        //             * (size_t)omp_get_thread_num()),
        //         lam, 1., lam_last,
        //         (FPnum*)NULL,
        //         bufferBtB, cnt_NA[ix], 0,
        //         (FPnum*)NULL, false,
        //         use_cg, max_cg_steps,
        //         false
        //     );
        // }
        size_t nvars = (size_t)m * (size_t)lda - (size_t)(lda-k);
        size_t m_lbfgs = 4;
        size_t past = 0;
        FPnum *restrict buffer_lbfgs = buffer_FPnum + (size_t)m*(size_t)n;
        lbfgs_parameter_t lbfgs_params = {
            m_lbfgs, 1e-5, past, 1e-5,
            75, LBFGS_LINESEARCH_MORETHUENTE, 20,
            1e-20, 1e20, 1e-4, 0.9, 0.9, 1.0e-16,
            0.0, 0, -1,
        };
        lbfgs_progress_t callback = (lbfgs_progress_t)NULL;
        if (is_first_iter)
            set_to_zero(A, nvars, nthreads);
        if (!do_B) {
            data_fun_grad_Adense data = {
                lda,
                B, ldb,
                m, n, k,
                Xfull, weight,
                lam, 1., lam_last,
                nthreads,
                buffer_FPnum
            };
            lbfgs(
                nvars,
                A,
                (FPnum*)NULL,
                wrapper_fun_grad_Adense,
                callback,
                (void*) &data,
                &lbfgs_params,
                buffer_lbfgs,
                buffer_lbfgs_iter
            );
        }

        else {
            data_fun_grad_Bdense data = {
                B, ldb,
                lda,
                n, m, k,
                Xfull, weight,
                lam, 1., lam_last,
                nthreads,
                buffer_FPnum
            };
            lbfgs(
                nvars,
                A,
                (FPnum*)NULL,
                wrapper_fun_grad_Bdense,
                callback,
                (void*) &data,
                &lbfgs_params,
                buffer_lbfgs,
                buffer_lbfgs_iter
            );
        }
    }

    /* Case 3: X is sparse, with missing-as-zero, and no weights.
       Here can also use one Cholesky for all rows at once. */
    else if (Xfull == NULL && NA_as_zero && weight == NULL)
    {
        FPnum *restrict bufferBtB = buffer_FPnum;
        cblas_tsyrk(CblasRowMajor, CblasUpper, CblasTrans,
                    k, n,
                    1., B, ldb,
                    0., bufferBtB, k);
        add_to_diag(bufferBtB, lam, k);
        if (lam_last != lam) bufferBtB[square(k)-1] += (lam_last - lam);
        if (lda == k)
            set_to_zero(A, (size_t)m*(size_t)k, 1);
        else
            for (size_t row = 0; row < (size_t)m; row++)
                memset(A + row*(size_t)lda, 0, (size_t)k*sizeof(FPnum));
        tgemm_sp_dense(
            m, k, 1.,
            Xcsr_p, Xcsr_i, Xcsr,
            B, (size_t)ldb,
            A, (size_t)lda,
            nthreads
        );
        tposv_(&uplo, &k, &m,
               bufferBtB, &k,
               A, &lda,
               &ignore);
    }

    /* Case 4: X is sparse, with non-present as NA, or with weights.
       This is the expected case for most situations. */
    else
    {
        /* When NAs are treated as zeros, can use a precomputed t(B)*B */
        FPnum *restrict bufferBtB = NULL;
        if (Xfull == NULL && NA_as_zero && (!use_cg || weight != NULL))
        {
            bufferBtB = buffer_FPnum;
            buffer_FPnum += square(k);
            cblas_tsyrk(CblasRowMajor, CblasUpper, CblasTrans,
                        k, n,
                        1., B, ldb,
                        0., bufferBtB, k);
            if (!(use_cg && Xfull == NULL && NA_as_zero))
            {
                add_to_diag(bufferBtB, lam, k);
                if (lam_last != lam) bufferBtB[square(k)-1] += (lam_last - lam);
            }
        }

        size_t size_buffer = square(k);
        if (use_cg)
            size_buffer = max2(size_buffer, (size_t)(3 * k));
        if (use_cg && Xfull == NULL)
            size_buffer = (size_t)(3 * k) + (size_t)(NA_as_zero? n : 0);

        #pragma omp parallel for schedule(dynamic) num_threads(nthreads) \
                shared(A, lda, B, ldb, m, n, k, lam, lam_last, weight, cnt_NA, \
                       Xcsr_p, Xcsr_i, Xcsr, buffer_FPnum, NA_as_zero, \
                       bufferBtB, size_buffer, use_cg)
        for (size_t_for ix = 0; ix < (size_t)m; ix++)
            if (Xcsr_p[ix+(size_t)1] > Xcsr_p[ix])
                factors_closed_form(
                    A + ix*(size_t)lda, k,
                    B, n, ldb,
                    (FPnum*)NULL, false,
                    Xcsr +  Xcsr_p[ix], Xcsr_i +  Xcsr_p[ix],
                    Xcsr_p[ix+(size_t)1] - Xcsr_p[ix],
                    (weight == NULL)? ((FPnum*)NULL) : (weight + Xcsr_p[ix]),
                    buffer_FPnum + ((size_t)omp_get_thread_num() * size_buffer),
                    lam, 1., lam_last,
                    (FPnum*)NULL,
                    bufferBtB, 0, 0,
                    (FPnum*)NULL, NA_as_zero,
                    use_cg, max_cg_steps,
                    false
                );
    }
}

void optimizeA_implicit
(
    FPnum *restrict A, size_t lda,
    FPnum *restrict B, size_t ldb,
    int m, int n, int k,
    size_t Xcsr_p[], int Xcsr_i[], FPnum *restrict Xcsr,
    FPnum lam,
    int nthreads,
    bool use_cg, int max_cg_steps, bool force_set_to_zero,
    FPnum *restrict precomputedBtBw, /* <- will be calculated if not passed */
    FPnum *restrict buffer_FPnum
)
{
    if (precomputedBtBw == NULL)
    {
        precomputedBtBw = buffer_FPnum;
        buffer_FPnum += square(k);
        cblas_tsyrk(CblasRowMajor, CblasUpper, CblasTrans,
                    k, n,
                    1., B, (int)ldb,
                    0., precomputedBtBw, k);
        if (!use_cg)
            add_to_diag(precomputedBtBw, lam, k);
    }
    if (!use_cg || force_set_to_zero)
        set_to_zero(A, (size_t)m*(size_t)k - (lda-(size_t)k), nthreads);
    size_t size_buffer = use_cg? (3 * k) : (square(k));

    int ix = 0;
    #pragma omp parallel for schedule(dynamic) num_threads(nthreads) \
            shared(A, B, lda, ldb, m, n, k, lam, \
                   Xcsr, Xcsr_i, Xcsr_p, precomputedBtBw, buffer_FPnum, \
                   size_buffer, use_cg)
    for (ix = 0; ix < m; ix++)
        factors_implicit(
            A + (size_t)ix*lda, k,
            B, ldb,
            Xcsr + Xcsr_p[ix], Xcsr_i + Xcsr_p[ix],
            Xcsr_p[ix+(size_t)1] - Xcsr_p[ix],
            lam,
            precomputedBtBw, 0,
            false, use_cg, max_cg_steps,
            buffer_FPnum + ((size_t)omp_get_thread_num() * size_buffer),
            false
        );
}

int initialize_biases
(
    FPnum *restrict glob_mean, FPnum *restrict biasA, FPnum *restrict biasB,
    bool user_bias, bool item_bias,
    FPnum lam_user, FPnum lam_item,
    int m, int n,
    int m_bias, int n_bias,
    int ixA[], int ixB[], FPnum *restrict X, size_t nnz,
    FPnum *restrict Xfull, FPnum *restrict Xtrans,
    size_t Xcsr_p[], int Xcsr_i[], FPnum *restrict Xcsr,
    size_t Xcsc_p[], int Xcsc_i[], FPnum *restrict Xcsc,
    int nthreads
)
{
    size_t m_by_n = (size_t)m * (size_t)n;
    #if defined(_OPENMP) && \
                ( (_OPENMP < 200801)  /* OpenMP < 3.0 */ \
                  || defined(_WIN32) || defined(_WIN64) \
                )
    long long ix, row, col;
    #endif

    size_t *buffer_cnt = (size_t*)calloc(max2(m,n), sizeof(size_t));
    if (buffer_cnt == NULL && (Xfull != NULL || Xcsr == NULL)) return 1;

    /* First calculate the global mean */
    double xsum = 0.;
    size_t cnt = 0;
    if (Xfull != NULL)
    {
        #ifdef _OPENMP
        if (nthreads >= 8)
        {
            #pragma omp parallel for schedule(static) num_threads(nthreads) \
                    reduction(+:xsum,cnt) shared(Xfull, m_by_n)
            for (size_t_for ix = 0; ix < m_by_n; ix++) {
                xsum += (!isnan(Xfull[ix]))? (Xfull[ix]) : (0);
                cnt += !isnan(Xfull[ix]);
            }
            *glob_mean = (FPnum)(xsum / (double)cnt);
        }

        else
        #endif
        {
            for (size_t ix = 0; ix < m_by_n; ix++) {
                if (!isnan(Xfull[ix])) {
                    xsum += (Xfull[ix] - xsum) / (double)(++cnt);
                }
            }
            *glob_mean = xsum;
        }
    }

    else
    {
        #ifdef _OPENMP
        if (nthreads >= 4)
        {
            #pragma omp parallel for schedule(static) num_threads(nthreads) \
                    reduction(+:xsum) shared(X, nnz)
            for (size_t_for ix = 0; ix < nnz; ix++)
                xsum += X[ix];
            *glob_mean = (FPnum)(xsum / (double)nnz);
        }

        else
        #endif
        {
            for (size_t ix = 0; ix < nnz; ix++) {
                xsum += (X[ix] - xsum) / (double)(++cnt);
            }
            *glob_mean = xsum;
        }
    }

    /* Now center X in-place */
    if (Xfull != NULL) {
        for (size_t_for ix = 0; ix < m_by_n; ix++)
            Xfull[ix] = isnan(Xfull[ix])? (NAN) : (Xfull[ix] - *glob_mean);
        if (Xtrans != NULL) {
            for (size_t_for ix = 0; ix < m_by_n; ix++)
                Xtrans[ix] = isnan(Xtrans[ix])? (NAN):(Xtrans[ix] - *glob_mean);
        }
    } else if (Xcsr != NULL) {
        for (size_t_for ix = 0; ix < nnz; ix++) {
            Xcsr[ix] -= *glob_mean;
            Xcsc[ix] -= *glob_mean;
        }
    } else {
        for (size_t_for ix = 0; ix < nnz; ix++)
            X[ix] -= *glob_mean;
    }

    /* Note: the original papers suggested starting these values by
       obtaining user biases first, then item biases, but I've found
       that doing it the other way around leads to better results
       with the ALS method **when** the ALS method updates the main matrices
       in that same order (which this software does, unlike most other
       implementations). Thus, both the ALS procedure and this function
       make updates over items first and users later. */

    /* Calculate item biases, but don't apply them to X */
    if (item_bias)
    {
        set_to_zero(biasB, n_bias, 1);
        if (Xtrans != NULL)
        {
            #pragma omp parallel for schedule(static) num_threads(nthreads) \
                    shared(m, n, Xtrans, biasB, buffer_cnt)
            for (size_t_for col = 0; col < (size_t)n; col++)
                for (size_t row = 0; row < (size_t)m; row++) {
                    biasB[col] += (!isnan(Xtrans[row + col*(size_t)m]))?
                                   (Xtrans[row + col*(size_t)m]) : (0.);
                    buffer_cnt[col] += !isnan(Xtrans[row + col*(size_t)m]);
                }

        }

        else if (Xfull != NULL)
        {
            for (size_t row = 0; row < (size_t)m; row++)
                for (size_t col = 0; col < (size_t)n; col++) {
                    biasB[col] += (!isnan(Xfull[col + row*(size_t)n]))?
                                   (Xfull[col + row*(size_t)n]) : (0.);
                    buffer_cnt[col] += !isnan(Xfull[col + row*(size_t)n]);
                }
        }

        else if (Xcsc != NULL)
        {
            #pragma omp parallel for schedule(dynamic) num_threads(nthreads) \
                    shared(n, Xcsc_p, Xcsc, biasB)
            for (size_t_for col = 0; col < (size_t)n; col++)
                for (size_t ix = Xcsc_p[col]; ix < Xcsc_p[col+(size_t)1]; ix++)
                    biasB[col] += (Xcsc[ix] - biasB[col])
                                   / ((double)(ix - Xcsc_p[col] +(size_t)1)
                                      + lam_item);
        }

        else
        {
            for (size_t ix = 0; ix < nnz; ix++) {
                biasB[ixB[ix]] += X[ix];
                buffer_cnt[ixB[ix]]++;
            }
        }

        if (Xfull != NULL || Xcsc == NULL)
            for (int ix = 0; ix < n; ix++)
                biasB[ix] /= ((double)buffer_cnt[ix] + lam_item);

        for (int ix = 0; ix < n; ix++)
            biasB[ix] = (!isnan(biasB[ix]))? biasB[ix] : 0.;
    }

    /* Finally, user biases */
    if (user_bias)
    {
        set_to_zero(biasA, m_bias, 1);
        if (item_bias) memset(buffer_cnt, 0, (size_t)m*sizeof(size_t));

        if (Xfull != NULL)
        {
            #pragma omp parallel for schedule(static) num_threads(nthreads) \
                    shared(m, n, Xfull, biasA, biasB, buffer_cnt)
            for (size_t_for row = 0; row < (size_t)m; row++)
                for (size_t col = 0; col < (size_t)n; col++) {
                    biasA[row] += (!isnan(Xfull[col + row*(size_t)n]))?
                                   (Xfull[col + row*(size_t)n]
                                     - (item_bias? biasB[col] : 0.))
                                   : (0.);
                    buffer_cnt[row] += !isnan(Xfull[col + row*(size_t)n]);
                }
        }

        else if (Xcsr != NULL)
        {
            #pragma omp parallel for schedule(dynamic) num_threads(nthreads) \
                    shared(m, Xcsr_p, Xcsr_i, Xcsr, biasA, biasB, item_bias)
            for (size_t_for row = 0; row < (size_t)m; row++)
                for (size_t ix = Xcsr_p[row]; ix < Xcsr_p[row+(size_t)1]; ix++)
                    biasA[row] += (Xcsr[ix]
                                    - (item_bias? (biasB[Xcsr_i[ix]]) : (0.))
                                    - biasA[row])
                                   / ((double)(ix - Xcsr_p[row] +(size_t)1)
                                      + lam_user);
        }

        else
        {
            for (size_t ix = 0; ix < nnz; ix++) {
                biasA[ixA[ix]] += X[ix] - (item_bias? (biasB[ixB[ix]]) : (0.));
                buffer_cnt[ixA[ix]]++;
            }
        }

        if (Xfull != NULL || Xcsr == NULL)
            for (int ix = 0; ix < m; ix++)
                biasA[ix] /= ((double)buffer_cnt[ix] + lam_user);

        for (int ix = 0; ix < m; ix++)
            biasA[ix] = (!isnan(biasA[ix]))? biasA[ix] : 0.;
    }

    free(buffer_cnt);
    return 0;
}

int center_by_cols
(
    FPnum *restrict col_means,
    FPnum *restrict Xfull, int m, int n,
    int ixA[], int ixB[], FPnum *restrict X, size_t nnz,
    size_t Xcsr_p[], int Xcsr_i[], FPnum *restrict Xcsr,
    size_t Xcsc_p[], int Xcsc_i[], FPnum *restrict Xcsc,
    int nthreads
)
{
    #if defined(_OPENMP) && \
                ( (_OPENMP < 200801)  /* OpenMP < 3.0 */ \
                  || defined(_WIN32) || defined(_WIN64) \
                )
    long long ix, ib;
    #endif
    int *restrict cnt_by_col = NULL;
    if (Xfull != NULL || Xcsc == NULL) {
        cnt_by_col = (int*)calloc(n, sizeof(int));
        if (cnt_by_col == NULL) return 1;
    }
    set_to_zero(col_means, n, 1);

    if (Xfull != NULL)
    {
        for (size_t row = 0; row < (size_t)m; row++)
            for (size_t col = 0; col < (size_t)n; col++) {
                col_means[col] += (!isnan(Xfull[col + row*(size_t)n]))?
                                   (Xfull[col + row*(size_t)n]) : (0.);
                cnt_by_col[col] += !isnan(Xfull[col + row*(size_t)n]);
            }
    }

    else if (Xcsc != NULL)
    {
        #pragma omp parallel for schedule(dynamic) num_threads(nthreads) \
                shared(n, Xcsc, Xcsc_p, col_means)
        for (size_t_for ib = 0; ib < (size_t)n; ib++)
            for (size_t ix = Xcsc_p[ib]; ix < Xcsc_p[ib+(size_t)1]; ix++)
                col_means[ib] += Xcsc[ix];
    }

    else if (Xcsr != NULL && X == NULL)
    {

        for (size_t ia = 0; ia < (size_t)m; ia++)
            for (size_t ix = Xcsr_p[ia]; ix < Xcsr_p[ia+(size_t)1]; ix++)
                col_means[Xcsr_i[ix]] += Xcsr[ix];
    }

    else
    {
        for (size_t ix = 0; ix < nnz; ix++) {
            col_means[ixB[ix]] += X[ix];
            cnt_by_col[ixB[ix]]++;
        }
    }

    /* -------- */
    if (Xfull != NULL || Xcsc == NULL)
        for (size_t ix = 0; ix < (size_t)n; ix++)
            col_means[ix] /= (double)cnt_by_col[ix];
    else
        for (size_t ix = 0; ix < (size_t)n; ix++)
            col_means[ix] /= (double)(Xcsc_p[ix+(size_t)1] - Xcsc_p[ix]);
    /* -------- */

    if (Xfull != NULL)
    {
        for (size_t row = 0; row < (size_t)m; row++)
            for (size_t col = 0; col < (size_t)n; col++)
                Xfull[col + row*(size_t)n] -= col_means[col];
    }

    else if (Xcsc != NULL || Xcsr != NULL)
    {
        if (Xcsc != NULL)
        {
            #pragma omp parallel for schedule(dynamic) num_threads(nthreads) \
                    shared(Xcsc, Xcsc_p, n, col_means)
            for (size_t_for ib = 0; ib < (size_t)n; ib++)
                for (size_t ix = Xcsc_p[ib]; ix < Xcsc_p[ib+(size_t)1]; ix++)
                    Xcsc[ix] -= col_means[ib];
        }

        if (Xcsr != NULL)
        {
            if (X != NULL)
                for (size_t ix = 0; ix < nnz; ix++)
                    Xcsr[ix] -= col_means[Xcsr_i[ix]];
            else
                for (size_t ia = 0; ia < (size_t)m; ia++)
                    for (size_t ix = Xcsr_p[ia]; ix < Xcsr_p[ia+(size_t)1];ix++)
                        Xcsr[ix] -= col_means[Xcsr_i[ix]];
        }
    }

    else
    {
        nthreads = cap_to_4(nthreads);
        #pragma omp parallel for schedule(static) num_threads(nthreads) \
                shared(nnz, X, col_means, ixB)
        for (size_t_for ix = 0; ix < nnz; ix++)
            X[ix] -= col_means[ixB[ix]];

    }

    free(cnt_by_col);
    return 0;
}

void predict_multiple
(
    FPnum *restrict A, int k_user,
    FPnum *restrict B, int k_item,
    FPnum *restrict biasA, FPnum *restrict biasB,
    FPnum glob_mean,
    int k, int k_main,
    int predA[], int predB[], size_t nnz,
    FPnum *restrict outp,
    int nthreads
)
{
    size_t lda = (size_t)k_user + (size_t)k + (size_t)k_main;
    size_t ldb = (size_t)k_item + (size_t)k + (size_t)k_main;
    A += k_user;
    B += k_item;
    #if defined(_OPENMP) && \
                ( (_OPENMP < 200801)  /* OpenMP < 3.0 */ \
                  || defined(_WIN32) || defined(_WIN64) \
                )
    long long ix;
    #endif
    #pragma omp parallel for schedule(static) num_threads(nthreads) \
            shared(A, B, outp, nnz, predA, predB, lda, ldb, k)
    for (size_t_for ix = 0; ix < nnz; ix++)
        outp[ix] = cblas_tdot(k, A + (size_t)predA[ix]*lda, 1,
                              B + (size_t)predB[ix]*ldb, 1)
                    + ((biasA != NULL)? biasA[predA[ix]] : 0.)
                    + ((biasB != NULL)? biasB[predB[ix]] : 0.)
                    + glob_mean;
}

int cmp_int(const void *a, const void *b)
{
    return *((int*)a) - *((int*)b);
}

FPnum *ptr_FPnum_glob = NULL;
// #pragma omp threadprivate(ptr_FPnum_glob)
// ptr_FPnum_glob = NULL;
int cmp_argsort(const void *a, const void *b)
{
    FPnum v1 = ptr_FPnum_glob[*((int*)a)];
    FPnum v2 = ptr_FPnum_glob[*((int*)b)];
    return (v1 == v2)? 0 : ((v1 < v2)? 1 : -1);
}

int topN
(
    FPnum *restrict a_vec, int k_user,
    FPnum *restrict B, int k_item,
    FPnum *restrict biasB,
    FPnum glob_mean, FPnum biasA,
    int k, int k_main,
    int *restrict include_ix, int n_include,
    int *restrict exclude_ix, int n_exclude,
    int *restrict outp_ix, FPnum *restrict outp_score,
    int n_top, int n, int nthreads
)
{
    if (include_ix != NULL && exclude_ix != NULL) {
        fprintf(stderr, "Cannot pass both 'include_ix' and 'exclude_ix'.\n");
        #ifndef _FOR_R
        fflush(stderr);
        #endif
        return 2;
    }
    if (n_top == 0) {
        fprintf(stderr, "'n_top' must be greater than zero.\n");
        #ifndef _FOR_R
        fflush(stderr);
        #endif
        return 2;
    }
    if (n_exclude > n-n_top) {
        fprintf(stderr, "Number of rankeable entities is less than 'n_top'\n");
        #ifndef _FOR_R
        fflush(stderr);
        #endif
        return 2;
    }
    if (n_include > n) {
        fprintf(stderr, "Number of entities to include is larger than 'n'.\n");
        #ifndef _FOR_R
        fflush(stderr);
        #endif
        return 2;
    }

    int ix = 0;

    int retval = 0;
    int k_pred = k + k_main;
    int k_totB = k_item + k + k_main;
    size_t n_take = (include_ix != NULL)?
                     (size_t)n_include :
                     ((exclude_ix == NULL)? (size_t)n : (size_t)(n-n_exclude) );

    FPnum *restrict buffer_scores = NULL;
    int *restrict buffer_ix = NULL;
    int *restrict buffer_mask = NULL;
    a_vec += k_user;

    if (include_ix != NULL) {
        buffer_ix = include_ix;
    }

    else {
        buffer_ix = (int*)malloc((size_t)n*sizeof(int));
        if (buffer_ix == NULL) { retval = 1; goto cleanup; }
        for (int ix = 0; ix < n; ix++) buffer_ix[ix] = ix;
    }

    if (exclude_ix != NULL)
    {
        int move_to = n-1;
        int temp;
        if (!check_is_sorted(exclude_ix, n_exclude))
            qsort(exclude_ix, n_exclude, sizeof(int), cmp_int);

        for (int ix = n_exclude-1; ix >= 0; ix--) {
            temp = buffer_ix[move_to];
            buffer_ix[move_to] = exclude_ix[ix];
            buffer_ix[exclude_ix[ix]] = temp;
            move_to--;
        }
    }

    /* Case 1: there is a potentially small number of items to include.
       Here can produce predictons only for those, then make
       an argsort with doubly-masked indices. */
    if (include_ix != NULL)
    {
        buffer_scores = (FPnum*)malloc((size_t)n_include*sizeof(FPnum));
        buffer_mask = (int*)malloc((size_t)n_include*sizeof(int));
        if (buffer_scores == NULL || buffer_mask == NULL) {
            retval = 1;
            goto cleanup;
        }
        #pragma omp parallel for schedule(static) num_threads(nthreads) \
                shared(a_vec, B, k_pred, k_item, n_include, k_totB, \
                       include_ix, biasB, buffer_scores)
        for (ix = 0; ix < n_include; ix++) {
            buffer_scores[ix] = cblas_tdot(k_pred, a_vec, 1,
                                           B + k_item + (size_t)include_ix[ix]
                                                * (size_t)k_totB, 1)
                                + ((biasB != NULL)? biasB[include_ix[ix]] : 0.);
        }
        for (int ix = 0; ix < n_include; ix++)
            buffer_mask[ix] = ix;
    }

    /* Case 2: there is a large number of items to exclude.
       Here can also produce predictions only for the included ones
       and then make a full or partial argsort. */
    else if (exclude_ix != NULL && (double)n_exclude > (double)n/20)
    {
        buffer_scores = (FPnum*)malloc(n_take*sizeof(FPnum));
        buffer_mask = (int*)malloc(n_take*sizeof(int));
        if (buffer_scores == NULL || buffer_mask == NULL) {
            retval = 1;
            goto cleanup;
        }
        #pragma omp parallel for schedule(static) num_threads(nthreads) \
                shared(a_vec, B, k_pred, k_item, n_take, k_totB, \
                       buffer_ix, biasB, buffer_scores)
        for (ix = 0; ix < (int)n_take; ix++)
            buffer_scores[ix] = cblas_tdot(k_pred, a_vec, 1,
                                           B + k_item + (size_t)buffer_ix[ix]
                                                * (size_t)k_totB, 1)
                                + ((biasB != NULL)? biasB[buffer_ix[ix]] : 0.);
        for (int ix = 0; ix < (int)n_take; ix++)
            buffer_mask[ix] = ix;
    }

    /* General case: make predictions for all the entries, then
       a partial argsort (this is faster since it makes use of
       optimized BLAS gemv, but it's not memory-efficient) */
    else
    {
        buffer_scores = (FPnum*)malloc((size_t)n*sizeof(FPnum));
        if (buffer_scores == NULL) { retval = 1; goto cleanup; }
        cblas_tgemv(CblasRowMajor, CblasNoTrans,
                    n, k_pred,
                    1., B + k_item, k_totB,
                    a_vec, 1,
                    0., buffer_scores, 1);
        if (biasB != NULL)
            cblas_taxpy(n, 1., biasB, 1, buffer_scores, 1);
    }

    /* If there is no double-mask for indices, do a partial argsort */
    ptr_FPnum_glob = buffer_scores;
    if (buffer_mask == NULL)
    {
        /* If the number of elements is very small, it's faster to
           make a full argsort, taking advantage of qsort's optimizations */
        if (n_take <= 50 || n_take >= (double)n*0.75)
        {
            qsort(buffer_ix, n_take, sizeof(int), cmp_argsort);
        }

        /* Otherwise, do a proper partial sort */
        else
        {
            qs_argpartition(buffer_ix, buffer_scores, n_take, n_top);
            qsort(buffer_ix, n_top, sizeof(int), cmp_argsort);
        }

        memcpy(outp_ix, buffer_ix, (size_t)n_top*sizeof(int));
    }

    /* Otherwise, do a partial argsort with doubly-indexed arrays */
    else
    {
        if (n_take <= 50 || n_take >= (double)n*0.75)
        {
            qsort(buffer_mask, n_take, sizeof(int), cmp_argsort);
        }

        else
        {
            qs_argpartition(buffer_mask, buffer_scores, n_take, n_top);
            qsort(buffer_mask, n_top, sizeof(int), cmp_argsort);
        }

        for (int ix = 0; ix < n_top; ix++)
                outp_ix[ix] = buffer_ix[buffer_mask[ix]];
    }
    ptr_FPnum_glob = NULL;

    /* If scores were requested, need to also output those */
    if (outp_score != NULL)
    {
        glob_mean += biasA;
        if (buffer_mask == NULL)
            for (int ix = 0; ix < n_top; ix++)
                outp_score[ix] = buffer_scores[outp_ix[ix]] + glob_mean;
        else
            for (int ix = 0; ix < n_top; ix++)
                outp_score[ix] = buffer_scores[buffer_mask[ix]] + glob_mean;
    }

    cleanup:
        free(buffer_scores);
        if (include_ix == NULL)
            free(buffer_ix);
        free(buffer_mask);
    if (retval == 1) return retval;
    return 0;
}

int fit_most_popular
(
    FPnum *restrict biasA, FPnum *restrict biasB,
    FPnum *restrict glob_mean,
    FPnum lam_user, FPnum lam_item,
    FPnum alpha,
    int m, int n,
    int ixA[], int ixB[], FPnum *restrict X, size_t nnz,
    FPnum *restrict Xfull,
    FPnum *restrict weight,
    bool implicit, bool adjust_weight,
    FPnum *restrict w_main_multiplier,
    int nthreads
)
{
    int retval = 0;
    int *restrict cnt_by_col = NULL;
    int *restrict cnt_by_row = NULL;
    float *restrict sum_by_col = NULL;
    float *restrict sum_by_row = NULL;

    if (implicit)
    {
        cnt_by_col = (int*)calloc((size_t)n, sizeof(int));
        sum_by_col = (float*)calloc((size_t)n, sizeof(float));
        if (cnt_by_col == NULL || sum_by_col == NULL)
        {
            retval = 1;
            goto cleanup;
        }

        if (Xfull != NULL)
        {
            for (size_t row = 0; row < (size_t)m; row++) {
                for (size_t col = 0; col < (size_t)n; col++) {
                    cnt_by_col[col] += !isnan(Xfull[col + row*(size_t)n]);
                    sum_by_col[col] += (!isnan(Xfull[col + row*(size_t)n]))?
                                        (Xfull[col + row*(size_t)n] +1.) : (0.);
                }
            }
        }

        else
        {
            for (size_t ix = 0; ix < nnz; ix++) {
                cnt_by_col[ixB[ix]]++;
                sum_by_col[ixB[ix]] += X[ix] + 1.;
            }
        }

        if (adjust_weight) {
            nnz = 0;
            for (int ix = 0; ix < n; ix++)
                nnz += (size_t)cnt_by_col[ix];
            *w_main_multiplier = (double)nnz / ((double)m * (double)n);
            lam_item /= *w_main_multiplier;
        }

        for (int ix = 0; ix < n; ix++)
            biasB[ix] = alpha * sum_by_col[ix]
                                / (alpha * sum_by_col[ix]
                                    + (double)(m - cnt_by_col[ix])
                                    + lam_item);

        goto cleanup;
    }


    if (biasA != NULL) {

        if (weight == NULL) {
            cnt_by_col = (int*)calloc((size_t)n, sizeof(int));
            cnt_by_row = (int*)calloc((size_t)m, sizeof(int));
        }
        else {
            sum_by_col = (float*)calloc((size_t)n, sizeof(float));
            sum_by_row = (float*)calloc((size_t)m, sizeof(float));
        }

        if ((cnt_by_col == NULL && sum_by_col == NULL) ||
            (cnt_by_row == NULL && sum_by_row == NULL))
        {
            retval = 1;
            goto cleanup;
        }
    }

    retval = initialize_biases(
        glob_mean, biasA, biasB,
        false, biasA == NULL,
        lam_user, lam_item,
        m, n,
        m, n,
        ixA, ixB, X, nnz,
        Xfull, (FPnum*)NULL,
        (size_t*)NULL, (int*)NULL, (FPnum*)NULL,
        (size_t*)NULL, (int*)NULL, (FPnum*)NULL,
        nthreads
    );
    if (retval == 1) return retval;


    if (biasA == NULL && !implicit)
    {
        goto cleanup;
    }

    if (Xfull != NULL)
    {
        if (weight == NULL)
            for (size_t row = 0; row < (size_t)m; row++) {
                for (size_t col = 0; col < (size_t)n; col++) {
                    cnt_by_row[row] += !isnan(Xfull[col + row*(size_t)n]);
                    cnt_by_col[col] += !isnan(Xfull[col + row*(size_t)n]);
                }
            }
        else
            for (size_t row = 0; row < (size_t)m; row++) {
                for (size_t col = 0; col < (size_t)n; col++) {
                    sum_by_row[row] += (!isnan(Xfull[col + row*(size_t)n]))?
                                        (weight[col + row*(size_t)n]) : (0.);
                    sum_by_col[col] += (!isnan(Xfull[col + row*(size_t)n]))?
                                        (weight[col + row*(size_t)n]) : (0.);
                }
            }
    }

    else
    {
        if (weight == NULL)
            for (size_t ix = 0; ix < nnz; ix++) {
                cnt_by_row[ixA[ix]]++;
                cnt_by_col[ixB[ix]]++;
            }
        else
            for (size_t ix = 0; ix < nnz; ix++) {
                sum_by_row[ixA[ix]] += weight[ix];
                sum_by_col[ixB[ix]] += weight[ix];
            }
    }

    set_to_zero(biasA, m, 1);
    set_to_zero(biasB, n, 1);

    for (int iter = 0; iter <= 5; iter++)
    {
        if (Xfull != NULL)
        {
            if (iter > 0)
                set_to_zero(biasB, n, 1);

            if (weight == NULL)
            {
                for (size_t row = 0; row < (size_t)m; row++)
                    for (size_t col = 0; col < (size_t)n; col++)
                        biasB[col] += (!isnan(Xfull[col + row*(size_t)n]))?
                                       (Xfull[col + row*(size_t)n] - biasA[row])
                                       : (0.);
                for (int ix = 0; ix < n; ix++)
                    biasB[ix] /= ((double)cnt_by_col[ix] + lam_item);
            }

            else
            {
                for (size_t row = 0; row < (size_t)m; row++)
                    for (size_t col = 0; col < (size_t)n; col++)
                        biasB[col] += (!isnan(Xfull[col + row*(size_t)n]))?
         weight[col + row*(size_t)n] * (Xfull[col + row*(size_t)n] - biasA[row])
                                       : (0.);
                for (int ix = 0; ix < n; ix++)
                    biasB[ix] /= (sum_by_col[ix] + lam_item);
            }

            for (int ix = 0; ix < n; ix++)
                biasB[ix] = (!isnan(biasB[ix]))? biasB[ix] : 0.;

            set_to_zero(biasA, m, 1);

            if (weight == NULL)
            {
                for (size_t row = 0; row < (size_t)m; row++)
                    for (size_t col = 0; col < (size_t)n; col++)
                        biasA[row] += (!isnan(Xfull[col + row*(size_t)n]))?
                                       (Xfull[col + row*(size_t)n] - biasB[col])
                                       : (0.);
                for (int ix = 0; ix < m; ix++)
                    biasA[ix] /= ((double)cnt_by_row[ix] + lam_user);
            }

            else
            {
                for (size_t row = 0; row < (size_t)m; row++)
                    for (size_t col = 0; col < (size_t)n; col++)
                        biasA[row] += (!isnan(Xfull[col + row*(size_t)n]))?
         weight[col + row*(size_t)n] * (Xfull[col + row*(size_t)n] - biasB[col])
                                       : (0.);
                for (int ix = 0; ix < m; ix++)
                    biasA[ix] /= (sum_by_row[ix] + lam_user);
            }

            for (int ix = 0; ix < m; ix++)
                biasA[ix] = (!isnan(biasA[ix]))? biasA[ix] : 0.;
        }

        else
        {
            if (iter > 0)
                set_to_zero(biasB, n, 1);

            if (weight == NULL)
            {
                for (size_t ix = 0; ix < nnz; ix++)
                    biasB[ixB[ix]] += (X[ix] - biasA[ixA[ix]]);
                for (int ix = 0; ix < n; ix++)
                    biasB[ix] /= ((double)cnt_by_col[ix] + lam_item);
            }

            else
            {
                for (size_t ix = 0; ix < nnz; ix++)
                    biasB[ixB[ix]] += weight[ix] * (X[ix] - biasA[ixA[ix]]);
                for (int ix = 0; ix < n; ix++)
                    biasB[ix] /= (sum_by_col[ix] + lam_item);
            }

            for (int ix = 0; ix < n; ix++)
                biasB[ix] = (!isnan(biasB[ix]))? biasB[ix] : 0.;

            set_to_zero(biasA, m, 1);

            if (weight == NULL)
            {
                for (size_t ix = 0; ix < nnz; ix++)
                    biasA[ixA[ix]] += (X[ix] - biasB[ixB[ix]]);
                for (int ix = 0; ix < m; ix++)
                    biasA[ix] /= ((double)cnt_by_row[ix] + lam_user);
            }

            else
            {
                for (size_t ix = 0; ix < nnz; ix++)
                    biasA[ixA[ix]] += weight[ix] * (X[ix] - biasB[ixB[ix]]);
                for (int ix = 0; ix < m; ix++)
                    biasA[ix] /= (sum_by_row[ix] + lam_user);
            }

            for (int ix = 0; ix < m; ix++)
                biasA[ix] = (!isnan(biasA[ix]))? biasA[ix] : 0.;
        }
    }

    cleanup:
        free(cnt_by_col);
        free(cnt_by_row);
        free(sum_by_col);
        free(sum_by_row);

    return retval;
}
