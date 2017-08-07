/*
 *      pvi Persistent Variational Inference
 */

 /*
This software estimates undirected graphical models of multiple sequence alignments.
*/

#include <stdlib.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <sys/time.h>
#include <assert.h>
#include <string.h>

/* Optionally include OpenMP with the -fopenmp flag */
#if defined(_OPENMP)
    #include <omp.h>
#endif

#include "include/pvi.h"
#include "include/bayes.h"
#include "include/inference.h"

/* Usage pattern */
const char *usage =
"pvi\n"
"\n"
"Usage:\n"
"      pvi [options] alignmentfile\n"
"      pvi -c couplingsfile alignmentfile\n"
"      pvi -o paramfile -c couplingsfile alignmentfile\n"
"      pvi [-h | --help]\n"
"      \n"
"    Required input:\n"
"      alignmentfile                    Multiple sequence alignment in FASTA format\n"
"\n"
"    Options, output:\n"
"      -c  --couplings  couplingsfile   Save coupling scores to file (text)\n"
"      -o  --output     paramfile       Save estimated parameters to file (binary)\n"
"\n"
"    Options, alignment processing:\n"
"      -s  --scale      <value>         Sequence weights: neighborhood weight [s > 0]\n"
"      -t  --theta      <value>         Sequence weights: neighborhood divergence [0 < t < 1]\n"
"\n"
"    Options, Maximum a posteriori estimation (L-BFGS, default):\n"
"      -eh --estimatelh                 Estimate L2 lambdas for fields (Bayesian)\n"
"      -ee --estimatele                 Estimate L2 lambdas for couplings (variance decomposition)\n"
"      -lh --lambdah    <value>         Set L2 lambda for fields (h_i)\n"
"      -le --lambdae    <value>         Set L2 lambda for couplings (e_ij)\n"
"\n"
"    Options, general:\n"
"      -a  --alphabet   alphabet        Alternative character set to use for analysis\n"
"      -f  --focus      identifier      Select only uppercase, non-gapped sites from a focus sequence\n"
"      -g  --gapignore                  Model sequence likelihoods only by coding, non-gapped portions\n"
"      -i  --independent                Estimate a site-independent model\n"
"      -m  --maxiter                    Maximum number of iterations\n"
"      -n  --ncores    [<number>|max]   Maximum number of threads to use in OpenMP\n"
"      -h  --help                       Usage\n\n";

/* Internal functions to MSARead */
void MSAReadSeq(char *seq, FILE *fpAli);
letter_t MSAReadCode(char c, char *alphabet, int nCodes);

numeric_t *DEBUGParams(alignment_t *ali);

/* Global verbosity & profiling options */
int verbose = 2;

/* Reference amino acid indexing */
const char *codesAA = "-ACDEFGHIKLMNPQRSTVWY";

/* Regularization default parameters */
const numeric_t REGULARIZATION_LAMBDA_H = 0.01;
const numeric_t REGULARIZATION_LAMBDA_E = 100.0;
const numeric_t REGULARIZATION_LAMBDA_GROUP = 0.0;
const numeric_t REWEIGHTING_THETA = 0.20;
const numeric_t REWEIGHTING_SCALE = 1.0;
const int ZERO_APC_PRIORS = 0;

int main(int argc, char **argv) {
    char *alignFile = NULL;
    char *outputFile = NULL;
    char *couplingsFile = NULL;

    /* Default options */
    options_t *options = (options_t *) malloc(sizeof(options_t));
    options->theta = REWEIGHTING_THETA;
    options->lambdaH = REGULARIZATION_LAMBDA_H;
    options->lambdaE = REGULARIZATION_LAMBDA_E;
    options->lambdaGroup = REGULARIZATION_LAMBDA_GROUP;
    options->scale = REWEIGHTING_SCALE;
    options->hyperprior = PRIOR_HALFCAUCHY;
    options->scaleH = 1.0;
    options->scaleE = 2000;
    options->noncentered = 0;
    options->zeroAPC = 0;
    options->bayesLH = 0;
    options->maxIter = 0;
    options->vSamples = 1;
    options->gChains = 20;
    options->gSweeps = 5;
    options->usePairs = 1;
    options->estimator = INFER_PLM;
    options->estimatorMAP = INFER_MAP_PLM;
    options->target = NULL;
    options->alphabet = (char *) codesAA;

    /* Print usage if no arguments */
    if (argc == 1) {
        fprintf(stderr, "%s", usage);
        exit(1);
    }

    /* Parse command line arguments */
    for (int arg = 1; arg < argc; arg++) {
        if ((arg < argc-1) && (strcmp(argv[arg], "--output") == 0
                    || strcmp(argv[arg], "-o") == 0)) {
            outputFile = argv[++arg];
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--alphabet") == 0
                    || strcmp(argv[arg], "-a") == 0)) {
            options->alphabet = argv[++arg];
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--couplings") == 0
                    || strcmp(argv[arg], "-c") == 0)) {
            couplingsFile = argv[++arg];
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--lambdah") == 0
                    || strcmp(argv[arg], "-lh") == 0)) {
            options->lambdaH = atof(argv[++arg]);
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--lambdae") == 0
                    || strcmp(argv[arg], "-le") == 0)) {
            options->lambdaE = atof(argv[++arg]);
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--lambdag") == 0
                    || strcmp(argv[arg], "-lg") == 0)) {
            options->lambdaGroup = atof(argv[++arg]);
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--theta") == 0
                    || strcmp(argv[arg], "-t") == 0)) {
            options->theta = atof(argv[++arg]);
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--scale") == 0
                    || strcmp(argv[arg], "-s") == 0)) {
            options->scale = atof(argv[++arg]);
        } else if ((arg < argc-1)  && (strcmp(argv[arg], "--maxiter") == 0
                    || strcmp(argv[arg], "-m") == 0)) {
            options->maxIter = atoi(argv[++arg]);
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--independent") == 0
                    || strcmp(argv[arg], "-i") == 0)) {
            options->usePairs = 0;
            fprintf(stderr, "Independent model not yet implemented\n");
            exit(0);
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--bayes") == 0
                    || strcmp(argv[arg], "-b") == 0)) {
            options->estimator = INFER_BAYES;
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--persist") == 0
                    || strcmp(argv[arg], "-p") == 0)) {
            options->estimator = INFER_MAP;
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--gapreduce") == 0
                    || strcmp(argv[arg], "-g") == 0)) {
            options->estimatorMAP = INFER_MAP_PLM_GAPREDUCE;
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--estimatele") == 0
                    || strcmp(argv[arg], "-ee") == 0)) {
            options->zeroAPC = 1;
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--estimatelh") == 0
                    || strcmp(argv[arg], "-eh") == 0)) {
            options->bayesLH = 1;
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--focus") == 0
                    || strcmp(argv[arg], "-f") == 0)) {
            options->target = argv[++arg];
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--variational") == 0
                    || strcmp(argv[arg], "-v") == 0)) {
            options->estimator = INFER_VBAYES;
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--vsamples") == 0
                    || strcmp(argv[arg], "-vs") == 0)) {
            /* Set the number of samples for KL gradient estimation */
            options->vSamples =  atoi(argv[++arg]);
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--gchains") == 0
                    || strcmp(argv[arg], "-gc") == 0)) {
            /* Set the number of MCMC chains for Gibbs sampling */
            options->gChains = atoi(argv[++arg]);
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--gsweeps") == 0
                    || strcmp(argv[arg], "-gs") == 0)) {
            /* Set the number of MCMC sweeps per chaing for Gibbs sampling */
            options->gSweeps = atoi(argv[++arg]);
        } else if ((arg < argc-1) && (strcmp(argv[arg], "--ncores") == 0
                    || strcmp(argv[arg], "-n") == 0)) {
            #if defined(_OPENMP)
                if (strcmp(argv[arg + 1], "max") == 0) {
                    int maxThreads = omp_get_max_threads();
                    /* Redundant, but serves as sanity check */
                    omp_set_num_threads(maxThreads);
                    fprintf(stderr, "OpenMP: Using %d of %d threads\n",
                        maxThreads, maxThreads);
                } else {
                    int numThreads = atoi(argv[arg + 1]);
                    int maxThreads = omp_get_max_threads();
                    if (numThreads >= 1 && numThreads <= maxThreads) {
                        omp_set_num_threads(numThreads);
                        fprintf(stderr, "OpenMP: Using %d of %d threads\n",
                            numThreads, maxThreads);
                    } else if (numThreads > maxThreads) {
                        omp_set_num_threads(maxThreads);
                        fprintf(stderr, "OpenMP: More threads requested than "
                            "available. Using %d of %d threads instead.\n",
                            maxThreads, maxThreads);
                    } else {
                        omp_set_num_threads(1);
                        fprintf(stderr, "OpenMP: Using 1 of %d threads\n",
                            maxThreads);
                    }
                }
                arg++;
            #else
                fprintf(stderr, "Error (-n/--ncores) only available when "
                    "compiled with OpenMP\n");
                exit(1);
            #endif
        } else if (strcmp(argv[arg], "--help") == 0
                    || strcmp(argv[arg], "-h") == 0) {
            fprintf(stderr, "%s", usage);
            exit(1);
        }
    }
    alignFile = argv[argc - 1];

    /* Read multiple seqence alignment */
    alignment_t *ali = MSARead(alignFile, options);

    /* Reweight sequences by inverse neighborhood density */
    MSAReweightSequences(ali, options->theta, options->scale);

    /* Compute sitwise and pairwise marginal distributions */
    MSACountMarginals(ali, options);

    /* Estimate effective sample size */
    if (options->theta >= 0 && options->theta <= 1)
        MSAEstimateSampleSize(ali, options);

    /* Infer model parameters */
    numeric_t *x = InferPairModel(ali, options);

    /* --------------------------------_DEBUG_--------------------------------*/
    /* Test set of parameters */
    // fprintf(stderr, "%d sites x %d states", ali->nSites, ali->nCodes);
    // for (int i = 0; i < ali->nSites; i++)
    //     for (int ai = 0; ai < ali->nCodes; ai++)
    //         xHi(i, ai) = (numeric_t) ai;
    // for (int i = 0; i < ali->nSites-1; i++)
    //     for (int j = i + 1; j < ali->nSites; j++)
    //         for (int ai = 0; ai < ali->nCodes; ai++)
    //             for (int aj = 0; aj < ali->nCodes; aj++)
    //                 xEij(i, j, ai, aj) = (numeric_t) (ai + aj);
    /* --------------------------------^DEBUG^--------------------------------*/

    /* Output estimated model parameters and (optionally) coupling scores */
    if (outputFile != NULL)
        if (options->estimator == INFER_VBAYES) {
            OutputParametersVBayes(outputFile, x, ali);
        } else {
            OutputParametersFull(outputFile, x, ali);
        }
    // if (outputFile != NULL)
    //     OutputParametersFullPLMDCA(outputFile, x, ali, options);
    if (couplingsFile != NULL)
        OutputCouplingScores(couplingsFile, x, ali, options);
}

alignment_t *MSARead(char *alignFile, options_t *options) {
    /* Read FASTA-formatted alignment */
    FILE *fpAli = NULL;
    if (alignFile != NULL) {
        fpAli = fopen(alignFile, "r");
    } else {
        fprintf(stderr, "Must specify alignment file: -a ALIGN_FILE\n");
        exit(1);
    }
    if (fpAli == NULL) {
        fprintf(stderr, "Error opening alignment file\n");
        exit(1);
    }

    /* Allocate alignment */
    alignment_t *ali = (alignment_t *) malloc(sizeof(alignment_t));
    ali->nSeqs = ali->nSites = ali->nCodes = 0;
    ali->alphabet = options->alphabet;
    ali->names = NULL;
    ali->sequences = NULL;
    ali->target = -1;
    ali->offsets = NULL;
    ali->nEff = 0;
    ali->weights = ali->fi = ali->fij = ali->gapi = ali->ungapij = NULL;
    ali->nParams = 0;

    /* Verify alignment dimensions and structure (first pass through file) */
    char name[BUFFER_SIZE];
    char seq[BUFFER_SIZE];
    fgetstr(name, fpAli);
    if (*name == '>') MSAReadSeq(seq, fpAli);
    ali->nCodes = strlen(ali->alphabet);
    ali->nSites = strlen(seq);
    ali->nSeqs = 1;
    while (!feof(fpAli)) {
        char c = fgetc(fpAli);
        if (c == '>') {
            /* Read name and sequence pair */
            fgetstr(name, fpAli);
            MSAReadSeq(seq, fpAli);
        } else {
            fprintf(stderr, "Error reading alignment:"
                            " sequences should start with >\n");
            exit(1);
        }

        /* Validate sequence length */
        if (strlen(seq) != ali->nSites) {
            fprintf(stderr,
                "Incompatible sequence length (%lu should be %d) for %s:\n%s\n",
                strlen(seq), ali->nSites, name, seq);
            exit(1);
        }
        ali->nSeqs++;
    }

    /* Encode full alignment block (second pass through file) */
    ali->sequences = (letter_t *)
        malloc(ali->nSites * ali->nSeqs * sizeof(letter_t));
    ali->names = (char **) malloc(ali->nSeqs * sizeof(char *));
    for (int s = 0; s < ali->nSeqs; s++)
        for (int i = 0; i < ali->nSites; i++) seq(s, i) = 0;
    for (int s = 0; s < ali->nSeqs; s++) ali->names[s] = NULL;
    rewind(fpAli);
    for (int s = 0; s < ali->nSeqs; s++) {
        /* >Name */
        getc(fpAli);
        fgetstr(name, fpAli);
        ali->names[s] = (char *) malloc((strlen(name) + 1) * sizeof(char));
        strcpy(ali->names[s], name);

        /* Sequence */
        MSAReadSeq(seq, fpAli);
        for (int i = 0; i < ali->nSites; i++)
            seq(s, i) = MSAReadCode(seq[i], ali->alphabet, ali->nCodes);
    }

    /* --------------------------------_DEBUG_--------------------------------*/
    /* Alignment to stderr */
    // for (int s = 0; s < 10; s++) {
    // // for (int s = 0; s < ali->nSeqs; s++) {
    //     for (int i = 0; i < ali->nSites; i++)
    //         if (seq(s, i) >= 0 && seq(s, i) < ali->nCodes) {
    //             fprintf(stderr, "%c", ali->alphabet[seq(s, i)]);
    //         } else if (seq(s, i) >= -ali->nCodes && seq(s, i) < 0) {
    //             fprintf(stderr, "%c",
    //                 tolower(ali->alphabet[seq(s, i) + ali->nCodes]));
    //         } else {
    //             fprintf(stderr, "_");
    //         }
    //     fprintf(stderr, "\n");
    // }
    // exit(0);
    /* --------------------------------^DEBUG^--------------------------------*/

    /* Focus mode: If a focus sequence (target) is provided, locate it */
    if (options->target != NULL) {
        for (int s = 0; s < ali->nSeqs; s++)
            if (strncmp(options->target, ali->names[s],
                strlen(options->target)) == 0) {
                if (ali->target >= 0) {
                    fprintf(stderr,
                        "Multiple sequences start with %s, picking sequence %d\n",
                        options->target, s + 1);
                } else {
                    ali->target = s;
                }
            }
        if (ali->target >= 0) {
            fprintf(stderr, "Found focus %s as sequence %d\n", options->target,
                ali->target + 1);
        } else {
            fprintf(stderr,
                "Could not find %s, proceeding without focus sequence\n",
                options->target);
        }
    }

    /* Always discard any sequences (rows) with out-of-alphabet characters */
    int* seqValid = (int *) malloc(ali->nSeqs * sizeof(int));
    for (int s = 0; s < ali->nSeqs; s++) seqValid[s] = 0;
    for (int s = 0; s < ali->nSeqs; s++)
        for (int i = 0; i < ali->nSites; i++)
            if ((seq(s, i) >= -ali->nCodes) && (seq(s, i) < ali->nCodes))
                seqValid[s]++;
    int nValidSeqs = 0;
    for (int s = 0; s < ali->nSeqs; s++)
        if (seqValid[s] == ali->nSites) nValidSeqs++;
    fprintf(stderr, "%d valid sequences out of %d \n", nValidSeqs, ali->nSeqs);

    /* Focus mode: select only focus columns (criteria below) */
    int nValidSites = ali->nSites;
    int* siteValid = (int *) malloc(ali->nSites * sizeof(int));
    for (int i = 0; i < ali->nSites; i++) siteValid[i] = 1;
    if (ali->target >= 0) {
        for (int i = 0; i < ali->nSites; i++) {
            /* For proteins, remove lower case and gap columns */
            if ((ali->alphabet == codesAA) 
                && (seq(ali->target, i) < 0))
                siteValid[i] = 0;
            /* Discard gaps */
            if ((ali->alphabet == codesAA)
                || (options->estimatorMAP == INFER_MAP_PLM_GAPREDUCE))
                if (seq(ali->target, i) == 0) siteValid[i] = 0;
        }
        nValidSites = 0;
        for (int i = 0; i < ali->nSites; i++)
            if (siteValid[i] == 1) nValidSites++;
        fprintf(stderr,
            "%d sites out of %d\n", nValidSites, ali->nSites);
    } else {
        fprintf(stderr,
            "%d sites\n", ali->nSites);
    }

    /* Focus mode: parse region (NAME/START_IX-END_IX) and map offsets */
    int leftOffset = 0;
    if (ali->target >= 0) {
        char *focusName = ali->names[ali->target];
        /* Name should be immediately followed by '/' */
        if (strlen(focusName) > strlen(options->target) + 1
            && focusName[strlen(options->target)] == '/') {
            /* Attempt to read integer region start */
            int regLeft = strlen(options->target) + 1;
            int ix = 0;
            if (isdigit(focusName[regLeft])) {
                while (regLeft + ix < strlen(focusName)
                       && isdigit(focusName[regLeft + ix + 1])) ix++;
                int tens = 1;
                leftOffset = -1;
                for (int i = ix; i >= 0; i--) {
                    leftOffset += tens * (focusName[regLeft + i] - '0');
                    tens *= 10;
                }
                fprintf(stderr, "Region starts at %d\n", leftOffset + 1);
            } else {
                fprintf(stderr, "Error parsing region, assuming start at 1");
            }
        }

        /* Map the offsets */
        ali->offsets = (int *) malloc(nValidSites * sizeof(int));
        for (int i = 0; i < nValidSites; i++) ali->offsets[i] = i + 1;
        int ix = 0;
        for (int i = 0; i < ali->nSites; i++)
            if (siteValid[i] == 1) {
                ali->offsets[ix] = i + 1 + leftOffset;
                ix++;
            }

        /* Reposition the target for reduced alignment */
        int targetShift = -1;
        for (int i = 0; i <= ali->target; i++)
            if (seqValid[i] == ali->nSites) targetShift++;
        ali->target = targetShift;
    }

    /* Copy only selected rows and columns */
    if (nValidSeqs < ali->nSeqs || nValidSites < ali->nSites) {
        letter_t *seqsReduced = (letter_t *)
            malloc(nValidSites * nValidSeqs * sizeof(letter_t));
        for (int i = 0; i < nValidSites * nValidSeqs; i++) seqsReduced[i] = 0;
        int sx = 0;
        for (int s = 0; s < ali->nSeqs; s++)
            if (seqValid[s] == ali->nSites) {
                int ix = 0;
                for (int i = 0; i < ali->nSites; i++) {
                    if (siteValid[i] == 1) {
                        seqsReduced[ix + sx * nValidSites] = seq(s, i);
                        ix++;
                    }
                }
                sx++;
            }

        /* Reallocate alignment with reduced dimensions */
        free(ali->sequences);
        ali->nSeqs = nValidSeqs;
        ali->nSites = nValidSites;
        ali->sequences = (letter_t *)
            malloc(nValidSites * nValidSeqs * sizeof(letter_t));
        for (int i = 0; i < nValidSites * nValidSeqs; i++)
            ali->sequences[i] = 0;
        for (int s = 0; s < nValidSeqs; s++)
            for (int i = 0; i < nValidSites; i++)
                seq(s, i) = seqsReduced[i + s * nValidSites];
        free(seqsReduced);
    }

    /* Shift any lowercase codes back to uppercase */
    if (ali->alphabet == codesAA)
        for (int s = 0; s < ali->nSeqs; s++)
            for (int i = 0; i < ali->nSites; i++)
                if (seq(s, i) < 0) seq(s, i) += ali->nCodes;

    /* Intialize weights to 1.0 */
    ali->weights = (numeric_t *) malloc(ali->nSeqs * sizeof(numeric_t));
    for (int s = 0; s < ali->nSeqs; s++) ali->weights[s] = 1.0;
    ali->nEff = (numeric_t) ali->nSeqs;

    /* --------------------------------_DEBUG_--------------------------------*/
    /* Display offset map */
    // for (int i = 0; i < ali->nSites; i++) {
    //     fprintf(stderr, "%d : %d : %c\n", i + 1, ali->offsets[i],
    //             ali->alphabet[seq(ali->target, i)]);
    // }
    // exit(0);
    /* Display target */
    // for (int i = 0; i < ali->nSites; i++) {
    //     fprintf(stderr, "%c", ali->alphabet[seq(ali->target, i)]);
    // }
    // fprintf(stderr, "\n");
    // exit(0);
    /* --------------------------------^DEBUG^--------------------------------*/

    /* --------------------------------_DEBUG_--------------------------------*/
    // for (int s = 0; s < ali->nSeqs; s++) {
    //     fprintf(stderr, ">%s\n", ali->names[s]);
    //     for (int i = 0; i < ali->nSites; i++)
    //         fprintf(stderr, "%c", ali->alphabet[seq(s, i)]);
    //     fprintf(stderr, "\n");
    // }
    /* --------------------------------^DEBUG^--------------------------------*/
    return ali;
}

void MSAReadSeq(char *seq, FILE *fpAli) {
    /* Read sequence from the current line(s) */
    char buf[BUFFER_SIZE];
    char c = fgetc(fpAli);
    ungetc(c, fpAli);
    seq[0] = '\0';
    while (c != '>' && !feof(fpAli)) {
        fgetstr(buf, fpAli);
        strcat(seq, buf);
        c = fgetc(fpAli);
        ungetc(c, fpAli);
    }
}

letter_t MSAReadCode(char c, char *alphabet, int nCodes) {
    /* Encode a character as an integer between -nCodes and +nCodes
          In alphabet:                     store index           [0, nCodes - 1]
          Lowercase version of alphabet:   downshift by nCodes   [-nCodes, -1]
          Out of alphabet:                 store nCodes          [nCodes]
     */
    letter_t i = 0;

    /* Protein-specific treatment of '.' */
    if (alphabet == codesAA) if (c == '.') c = '-';

    /* Store lowercase characters as down-shifted by nCodes */
    while ((i < nCodes - 1) && toupper(c) != alphabet[i]) i++;
    if (c != alphabet[i] && toupper(c) == alphabet[i]) i -= nCodes;

    /* Encode out-of-alphabet characters by [nCodes] */
    if (i > 0 && toupper(c) != alphabet[i]) i = nCodes;
    return i;
}

void MSAReweightSequences(alignment_t *ali, numeric_t theta, numeric_t scale) {
    /* Reweight seqeuences by their inverse neighborhood size. Each sequence's
       weight is the inverse of the number of neighboring sequences with less
       than THETA percent divergence
    */
    for (int i = 0; i < ali->nSeqs; i++) ali->weights[i] = 1.0;

    /* Only apply reweighting if theta is on [0,1] */
    if (theta >= 0 && theta <= 1) {
        /* The neighborhood size of each sequence is the number of sequences 
           in the alignment within theta percent divergence */

        #if defined(_OPENMP)
        /* Naive parallelization is faster ignoring symmetry */
        #pragma omp parallel for
        for (int s = 0; s < ali->nSeqs; s++)
            for (int t = 0; t < ali->nSeqs; t++)
                if (s != t) {
                    int id = 0;
                    for (int n = 0; n < ali->nSites; n++)
                        id += (seq(s, n) == seq(t, n));
                    if (id >= ((1 - theta) * ali->nSites))
                        ali->weights[s] += 1.0;
                }
        #else
        /* For a single core, take advantage of symmetry */
        for (int s = 0; s < ali->nSeqs - 1; s++)
            for (int t = s + 1; t < ali->nSeqs; t++) {
                int id = 0;
                for (int n = 0; n < ali->nSites; n++)
                    id += (seq(s, n) == seq(t, n));
                if (id >= ((1 - theta) * ali->nSites)) {
                    ali->weights[s] += 1.0;
                    ali->weights[t] += 1.0;
                }
            }
        #endif

        /* Reweight sequences by the inverse of the neighborhood size */
        for (int i = 0; i < ali->nSeqs; i++)
            ali->weights[i] = 1.0 / ali->weights[i];
    }

    /* Scale sets the effective number of samples per neighborhood */
    for (int i = 0; i < ali->nSeqs; i++)
            ali->weights[i] *= scale;

    /* The effective number of sequences is then the sum of the weights */
    ali->nEff = 0;
    for (int i = 0; i < ali->nSeqs; i++) ali->nEff += ali->weights[i];

    if (theta >= 0 && theta <= 1) {
        fprintf(stderr, 
            "Neighborhood sample size: %.1f\t(%.0f%% identical neighborhood = %.3f samples)\n",
            ali->nEff, 100 * (1 - theta), scale);
    } else {
        fprintf(stderr,
            "Theta not between 0 and 1, no sequence reweighting applied\n");
    }
}

void MSACountMarginals(alignment_t *ali, options_t *options) {
    /* Compute first and second order marginal distributions, according to the 
       sequence weights
     */
    if (options->estimatorMAP == INFER_MAP_PLM_GAPREDUCE) {
        /* Condition the marginals on ungapped */
        ali->nCodes = strlen(ali->alphabet) - 1;

        /* Gap frequencies at each position */
        numeric_t Zinv = 1.0 / ali->nEff;
        ali->gapi = (numeric_t *) malloc(ali->nSites * sizeof(numeric_t));
        for (int i = 0; i < ali->nSites; i++) ali->gapi[i] = 0.0;
        for (int s = 0; s < ali->nSeqs; s++)
            for (int i = 0; i < ali->nSites; i++)
                    ali->gapi[i] += (seq(s, i) == 0) * ali->weights[s];
        for (int i = 0; i < ali->nSites; i++) ali->gapi[i] *= Zinv;

        /* Doubly ungapped frequencies at each pair of positions */
        int nPairs = ali->nSites * (ali->nSites - 1) / 2;
        ali->ungapij = (numeric_t *) malloc(nPairs * sizeof(numeric_t));
        for (int i = 0; i < nPairs; i++) ali->ungapij[i] = 0.0;
        for (int s = 0; s < ali->nSeqs; s++)
            for (int i = 0; i < ali->nSites - 1; i++)
                for (int j = i + 1; j < ali->nSites; j++)
                    ungapij(i, j) += (seq(s, i) > 0) 
                                   * (seq(s, j) > 0) 
                                   * ali->weights[s];
        for (int i = 0; i < nPairs; i++) ali->ungapij[i] *= Zinv;

        /* ------------------------------_DEBUG_------------------------------*/
        /* Gap frequencies */
        // for (int i = 0; i < ali->nSites; i++)
        //     fprintf(stderr, "%0.2f\t", ali->gapi[i]);
        // fprintf(stderr, "\n");
        // exit(0);
        /* Gap pair frequencies */
        // for (int i = 0; i < ali->nSites - 1; i++)
        //     for (int j = i + 1; j < ali->nSites; j++)
        //         fprintf(stderr, "%0.2f\t", ungapij(i, j));
        // fprintf(stderr, "\n");
        // exit(0);
        /* ------------------------------^DEBUG^------------------------------*/

        /* First-order ungapped marginals P_i(Ai) */
        int nFi = ali->nSites * ali->nCodes;
        ali->fi = (numeric_t *) malloc(nFi * sizeof(numeric_t));
        for (int i = 0; i < nFi; i++) ali->fi[i] = 0.0;

        for (int s = 0; s < ali->nSeqs; s++)
            for (int i = 0; i < ali->nSites; i++)
                if (seq(s, i) > 0)
                    fi(i, seq(s, i) - 1) += ali->weights[s];

        /* Second-order ungapped marginals P_ij(Ai, Aj) */
        int nFij = ali->nSites * (ali->nSites - 1) / 2 * ali->nCodes * ali->nCodes;
        ali->fij = (numeric_t *) malloc(nFij * sizeof(numeric_t));
        for (int i = 0; i < nFij; i++) ali->fij[i] = 0.0;

        for (int s = 0; s < ali->nSeqs; s++)
            for (int i = 0; i < ali->nSites - 1; i++)
                for (int j = i + 1; j < ali->nSites; j++)
                    if (seq(s, i) > 0) if(seq(s, j) > 0)
                        fij(i, j, seq(s, i) - 1, seq(s, j) - 1)
                            += ali->weights[s];

        /* Normalize conditional distributions */
        for (int i = 0; i < ali->nSites; i++) {
            double fsum = 0.0;
            for (int ai = 0; ai < ali->nCodes; ai++)
                fsum += fi(i, ai);
            double fsumInv = 1.0 / fsum;
            for (int ai = 0; ai < ali->nCodes; ai++)
                fi(i, ai) *= fsumInv;
        }
        for (int i = 0; i < ali->nSites - 1; i++)
            for (int j = i + 1; j < ali->nSites; j++) {
                double fsum = 0.0;
                for (int ai = 0; ai < ali->nCodes; ai++)
                    for (int aj = 0; aj < ali->nCodes; aj++)
                        fsum += fij(i, j, ai, aj);
                double fsumInv = 1.0 / fsum;
                for (int ai = 0; ai < ali->nCodes; ai++)
                    for (int aj = 0; aj < ali->nCodes; aj++)
                        fij(i, j, ai, aj) *= fsumInv;
            }

    /* --------------------------------_DEBUG_--------------------------------*/
    /* TEST CASE VALUES */
    // for (int i = 0; i < ali->nSites - 1; i++) {
    //     fi(i, 0) = i;
    //     fi(i, ali->nCodes - 2) = i;
    //     for (int j = i + 1; j < ali->nSites; j++) {
    //         fij(i, j, 0, 0) = 10;
    //         fij(i, j, ali->nCodes - 2, ali->nCodes - 2) = 10;
    //         fij(i, j, ali->nCodes - 2, 0) = 10;
    //         fij(i, j, 0, ali->nCodes - 2) = 10;
    //     }
    // }
    /* --------------------------------^DEBUG^--------------------------------*/

    } else {
        /* Compute regular marginals */
        numeric_t Zinv = 1.0 / ali->nEff;

        /* First-order marginals P_i(Ai) */
        int nFi = ali->nSites * ali->nCodes;
        ali->fi = (numeric_t *) malloc(nFi * sizeof(numeric_t));
        for (int i = 0; i < nFi; i++) ali->fi[i] = 0.0;

        for (int s = 0; s < ali->nSeqs; s++)
            for (int i = 0; i < ali->nSites; i++)
                fi(i, seq(s, i)) += ali->weights[s] * Zinv;

        /* Second-order marginals P_ij(Ai, Aj) */
        int nFij = ali->nSites * (ali->nSites - 1) / 2 * ali->nCodes * ali->nCodes;
        ali->fij = (numeric_t *) malloc(nFij * sizeof(numeric_t));
        for (int i = 0; i < nFij; i++) ali->fij[i] = 0.0;

        for (int s = 0; s < ali->nSeqs; s++)
            for (int i = 0; i < ali->nSites - 1; i++)
                for (int j = i + 1; j < ali->nSites; j++)
                    fij(i, j, seq(s, i), seq(s, j)) += ali->weights[s] * Zinv;
    }
}

void MSAEstimateSampleSize(alignment_t *ali, options_t *options) {
    /* Estimates effective sample size by a stochastic optimization procedure
        based on Miller-Maddow scaling
     */

    /* Compute the average MI of the data distribution */
    numeric_t avgMI = 0;
    for (int i = 0; i < ali->nSites - 1; i++)
        for (int j = i + 1; j < ali->nSites; j++) {
            numeric_t MI = 0;
            for (int ai = 0; ai < ali->nCodes; ai++)
                for (int aj = 0; aj < ali->nCodes; aj++)
                    if (fij(i, j, ai, aj) > 0)
                        MI += fij(i, j, ai, aj)
                              * (log(fij(i, j, ai, aj)) 
                                 - log(fi(i, ai))
                                 - log(fi(j, aj)));
            avgMI += MI;
        }
    avgMI *= 1.0 / ((numeric_t) (ali->nSites * (ali->nSites - 1) / 2));
    
    /* Use stochastic optimization to determine log(N) */
    InitRNG(42);
    numeric_t uncN = log(ali->nEff);
    int iterations = 1E3;
    int batchSize = 100;
    numeric_t learningRate = 10;

    /* Robbins-Monro root finding for <MI(sample)> - <MI>_N */
    numeric_t *F = (numeric_t *) malloc(sizeof(numeric_t) 
                                        * ali->nCodes * ali->nCodes);
    for (int t = 0; t < iterations; t++) {
        numeric_t sampleMI = 0;
        /* Sample the sample size */
        numeric_t Neff = exp(uncN);

        for (int batch = 0; batch < batchSize; batch++) {
            /* Pick random i & j */
            int i = RandomInt(ali->nSites);
            int j = i;
            while (j == i) j = RandomInt(ali->nSites);

            /* Adjust local effective sample size given gap statistics */
            numeric_t NeffLocal = Neff;
            if (options->estimatorMAP == INFER_MAP_PLM_GAPREDUCE)
                NeffLocal *= ungapij(i, j);
            int Nint = (int) (floor(NeffLocal) + (RandomUniform()
                            < (NeffLocal - floor(NeffLocal))));
            numeric_t floatNi = (numeric_t) Nint;

            /* Estimate the single-site distributions from counts */
            numeric_t *iC =
                (numeric_t *) malloc(sizeof(numeric_t) * ali->nCodes);
            numeric_t *jC =
                (numeric_t *) malloc(sizeof(numeric_t) * ali->nCodes);
            numeric_t *iP =
                (numeric_t *) malloc(sizeof(numeric_t) * ali->nCodes);
            numeric_t *jP =
                (numeric_t *) malloc(sizeof(numeric_t) * ali->nCodes);
            for (int a = 0; a < ali->nCodes; a++)
                iC[a] = round(floatNi * fi(i,a));
            for (int a = 0; a < ali->nCodes; a++)
                jC[a] = round(floatNi * fi(j,a));

            /* Sample null distribution given single-site counts */
            SampleCategoricalDistribution(iC, iP, ali->nCodes);
            SampleCategoricalDistribution(jC, jP, ali->nCodes);

            /* Bayes least squares estimator */
            // EstimateCategoricalDistribution(iC, iP, ali->nCodes);
            // EstimateCategoricalDistribution(jC, jP, ali->nCodes);

            /* Maximum likelihood estimator */
            // for (int a = 0; a < ali->nCodes; a++) iP[a] = fi(i,a);
            // for (int a = 0; a < ali->nCodes; a++) jP[a] = fi(j,a);

            /* Compute CDFs */
            numeric_t *iCDF =
                (numeric_t *) malloc(sizeof(numeric_t) * ali->nCodes);
            numeric_t *jCDF =
                (numeric_t *) malloc(sizeof(numeric_t) * ali->nCodes);
            iCDF[0] = iP[0];
            jCDF[0] = jP[0];
            for (int a = 1; a < ali->nCodes; a++) iCDF[a] = iCDF[a-1] + iP[a];
            for (int a = 1; a < ali->nCodes; a++) jCDF[a] = jCDF[a-1] + jP[a];

            /* Sample {Ai,Aj} N times */
            for (int a = 0; a < ali->nCodes * ali->nCodes; a++) F[a] = 0;
            numeric_t invN = 1.0 / ((numeric_t) Nint);
            for (int sx = 0; sx < Nint; sx++) {
                /* Sample si, sj */
                numeric_t Ui = RandomUniform();
                numeric_t Uj = RandomUniform();
                int si = 0;
                int sj = 0;
                while (Ui > iCDF[si]) si++;
                while (Uj > jCDF[sj]) sj++;
                F[si + ali->nCodes * sj] += invN;
            }

            /* Compute sample MI */
            numeric_t *iN =
                (numeric_t *) malloc(sizeof(numeric_t) * ali->nCodes);
            numeric_t *jN =
                (numeric_t *) malloc(sizeof(numeric_t) * ali->nCodes);
            for (int ai = 0; ai < ali->nCodes; ai++) {
                iN[ai] = F[ai];
                for (int aj = 1; aj < ali->nCodes; aj++)
                    iN[ai] += F[ai + ali->nCodes * aj];
            }
            for (int aj = 0; aj < ali->nCodes; aj++) {
                jN[aj] = F[ali->nCodes * aj];
                for (int ai = 1; ai < ali->nCodes; ai++)
                    jN[aj] += F[ai + ali->nCodes * aj];
            }
            numeric_t MIij = 0;
            for (int ai = 0; ai < ali->nCodes; ai++)
                for (int aj = 0; aj < ali->nCodes; aj++)
                    if (F[ai + ali->nCodes * aj] > 0)
                        MIij += F[ai + ali->nCodes * aj]
                              * (log(F[ai + ali->nCodes * aj]) 
                                 - log(iN[ai])
                                 - log(jN[aj]));

            /* Average the Mutual Information across the samples*/
            sampleMI += MIij / ((numeric_t) batchSize);

            /* ----------------------------_DEBUG_----------------------------*/
            /* Dump CDFs and sample to console */
            // fprintf(stderr, "Average Mutual Information: %f\n", avgMI);
            // fprintf(stderr, "\n Site %d:", i + 1);
            // for (int a = 0; a < ali->nCodes; a++)
            //     fprintf(stderr, "\t%c %.2f", ali->alphabet[a], iC[a]/ali->nEff);
            // fprintf(stderr, "\n Corr %d:", i + 1);
            // for (int a = 0; a < ali->nCodes; a++)
            //     fprintf(stderr, "\t%c %.2f", ali->alphabet[a], iP[a]);
            // fprintf(stderr, "\n Site %d:", j + 1);
            // for (int a = 0; a < ali->nCodes; a++)
            //     fprintf(stderr, "\t%c %.2f", ali->alphabet[a], jC[a]/ali->nEff);
            // fprintf(stderr, "\n Corr %d:", j + 1);
            // for (int a = 0; a < ali->nCodes; a++)
            //     fprintf(stderr, "\t%c %.2f", ali->alphabet[a], jP[a]);
            // fprintf(stderr, "\n\n");
            // for (int ai = 0; ai < ali->nCodes; ai++) {
            //     for (int aj = 0; aj < ali->nCodes; aj++)
            //          fprintf(stderr, "%.4f\t", F[ai + ali->nCodes * aj]);
            //     fprintf(stderr, "\n");
            // }
            // fprintf(stderr, "MI: %f\n", MIij);
            // /* MATLAB sanity check from dumped F
            //    MI_fact = F .* log(F ./ (sum(F,2) * sum(F,1))); 
            //    MI = sum(MI_fact(isfinite(MI_fact)))
            //  */
            // exit(0);
            /* ----------------------------^DEBUG^----------------------------*/
            free(iC);
            free(jC);
            free(iP);
            free(jP);
            free(iCDF);
            free(jCDF);
            free(iN);
            free(jN);
        }

        /* Report progress */
        if (t > 0 && t % 50 == 49)
            fprintf(stderr, "%8d\t%8.3f\t%8.2f\n", t + 1, sampleMI, Neff);

        /* Adjust N by a Robbins-Monro scheme */
        uncN += (sampleMI - avgMI) * (learningRate / (numeric_t) (t + 1));
    }
    free(F);

    /* Scale sets the effective number of samples per neighborhood */
    numeric_t ratio = exp(uncN) / ali->nEff;
    for (int i = 0; i < ali->nSeqs; i++) ali->weights[i] *= ratio;
    ali->nEff = exp(uncN);
    fprintf(stderr, 
            "Effective sample size: %.1f\t(%.0f%% identical neighborhood = %.3f samples)\n",
            ali->nEff, 100 * (1 - options->theta), ratio * options->scale);
}

#define OUTPUT_PRECISION float
void OutputParametersSite(char *outputFile, const numeric_t *x,
    alignment_t *ali) {
    FILE *fpOutput = NULL;
    fpOutput = fopen(outputFile, "w");
    if (fpOutput != NULL) {
        /* 1: nSites */
        fwrite(&(ali->nSites), sizeof(ali->nSites), 1, fpOutput);

        /* 2: (Focus mode only) target sequence */
        if (ali->target >= 0) {
            for (int i = 0; i < ali->nSites; i++) {
                char c = (char) ali->alphabet[seq(ali->target, i)];
                fwrite(&c, sizeof(char), 1, fpOutput);
            }
        } else {
            char c = ali->alphabet[0];
            for (int i = 0; i < ali->nSites; i++)
                fwrite(&c, sizeof(c), 1, fpOutput);
        }

        /* 3: (Focus mode only) offset map */
        if (ali->target >= 0) {
            for (int i = 0; i < ali->nSites; i++) {
                int ix = ali->offsets[i];
                fwrite(&ix, sizeof(ix), 1, fpOutput);
            }
        } else {
            for (int i = 0; i < ali->nSites; i++) {
                int ix = i + 1;
                fwrite(&ix, sizeof(ix), 1, fpOutput);
            }
        }

        /* 4,5: sitewise marginals fi, twice */
        for (int x = 0; x < 2; x++)
            for (int i = 0; i < ali->nSites; i++)
                for (int ai = 0; ai < ali->nCodes; ai++) {
                    OUTPUT_PRECISION f = (OUTPUT_PRECISION) fi(i, ai);
                    fwrite(&f, sizeof(f), 1, fpOutput);
                }

        /* 6: sitewise parameters hi */
        for (int i = 0; i < ali->nSites; i++)
            for (int ai = 0; ai < ali->nCodes; ai++) {
                OUTPUT_PRECISION h = (OUTPUT_PRECISION) xHi(i, ai);
                fwrite(&h, sizeof(h), 1, fpOutput);
            }

        fclose(fpOutput);
    } else {
        fprintf(stderr, "Error writing parameters\n");
        exit(1);
    }
}

void OutputParametersFull(char *outputFile, const numeric_t *x,
    alignment_t *ali) {
    FILE *fpOutput = NULL;
    fpOutput = fopen(outputFile, "w");
    if (fpOutput != NULL) {
        /* 1: nSites */
        fwrite(&(ali->nSites), sizeof(ali->nSites), 1, fpOutput);

        /* 2: nCodes */
        fwrite(&(ali->nCodes), sizeof(ali->nCodes), 1, fpOutput);

        /* 3: (Focus mode) target sequence */
        if (ali->target >= 0) {
            for (int i = 0; i < ali->nSites; i++) {
                char c = (char) ali->alphabet[seq(ali->target, i)];
                fwrite(&c, sizeof(char), 1, fpOutput);
            }
        } else {
            char c = ali->alphabet[0];
            for (int i = 0; i < ali->nSites; i++)
                fwrite(&c, sizeof(c), 1, fpOutput);
        }

        /* 4: (Focus mode) offset map */
        if (ali->target >= 0) {
            for (int i = 0; i < ali->nSites; i++) {
                int ix = ali->offsets[i];
                fwrite(&ix, sizeof(ix), 1, fpOutput);
            }
        } else {
            for (int i = 0; i < ali->nSites; i++) {
                int ix = i + 1;
                fwrite(&ix, sizeof(ix), 1, fpOutput);
            }
        }

        /* 5: sitewise marginals fi */
        for (int i = 0; i < ali->nSites; i++)
            for (int ai = 0; ai < ali->nCodes; ai++) {
                OUTPUT_PRECISION f = (OUTPUT_PRECISION) fi(i, ai);
                fwrite(&f, sizeof(f), 1, fpOutput);
            }

        /* 6: sitewise parameters hi */
        for (int i = 0; i < ali->nSites; i++)
            for (int ai = 0; ai < ali->nCodes; ai++) {
                OUTPUT_PRECISION h = (OUTPUT_PRECISION) xHi(i, ai);
                fwrite(&h, sizeof(h), 1, fpOutput);
            }

        /* 7: coupling marginals & parameters fij, hi~, hj~, eij */
        for (int i = 0; i < ali->nSites - 1; i++)
            for (int j = i + 1; j < ali->nSites; j++) {
                /* 7a: i, j dimensions */
                int ix = i + 1;
                int jx = j + 1;
                fwrite(&ix, sizeof(ix), 1, fpOutput);
                fwrite(&jx, sizeof(jx), 1, fpOutput);

                /* 7a: fij */
                for (int ai = 0; ai < ali->nCodes; ai++)
                    for (int aj = 0; aj < ali->nCodes; aj++) {
                        OUTPUT_PRECISION f =
                            (OUTPUT_PRECISION) fij(i, j, ai, aj);
                        fwrite(&f, sizeof(f), 1, fpOutput);
                    }

                /* 7e: eij */
                for (int ai = 0; ai < ali->nCodes; ai++)
                    for (int aj = 0; aj < ali->nCodes; aj++) {
                        OUTPUT_PRECISION e =
                            (OUTPUT_PRECISION) xEij(i, j, ai, aj);
                        fwrite(&e, sizeof(e), 1, fpOutput);
                    }
            }

        fclose(fpOutput);
    } else {
        fprintf(stderr, "Error writing parameters\n");
        exit(1);
    }
}

void OutputParametersVBayes(char *outputFile, const numeric_t *x,
    alignment_t *ali) {
    /* Output a variational (Gaussian) approximation to the full posterior 
       of parameters and hyperparameters of an undirected graphical model 
       with Gaussian priors over the couplings */

    /* Map into means and variance parameters */
    int n = ali->nParams + 2 + ali->nSites + ali->nSites * (ali->nSites - 1) / 2;
    int offset = 2 + ali->nSites + ali->nSites * (ali->nSites - 1) / 2;
    const numeric_t *mu = x;
    const numeric_t *sigma = &(x[n]);
    const numeric_t *xMu = &(mu[offset]);
    const numeric_t *xSig = &(sigma[offset]);
    const numeric_t *lambdaMu = &(mu[2]);
    const numeric_t *lambdaSig = &(sigma[2]);

    FILE *fpOutput = NULL;
    fpOutput = fopen(outputFile, "w");
    if (fpOutput != NULL) {
        /* 1: nSites */
        fwrite(&(ali->nSites), sizeof(ali->nSites), 1, fpOutput);

        /* 2: nCodes */
        fwrite(&(ali->nCodes), sizeof(ali->nCodes), 1, fpOutput);

        /* 3: (Focus mode) target sequence */
        if (ali->target >= 0) {
            for (int i = 0; i < ali->nSites; i++) {
                char c = (char) ali->alphabet[seq(ali->target, i)];
                fwrite(&c, sizeof(char), 1, fpOutput);
            }
        } else {
            char c = ali->alphabet[0];
            for (int i = 0; i < ali->nSites; i++)
                fwrite(&c, sizeof(c), 1, fpOutput);
        }

        /* 4: (Focus mode) offset map */
        if (ali->target >= 0) {
            for (int i = 0; i < ali->nSites; i++) {
                int ix = ali->offsets[i];
                fwrite(&ix, sizeof(ix), 1, fpOutput);
            }
        } else {
            for (int i = 0; i < ali->nSites; i++) {
                int ix = i + 1;
                fwrite(&ix, sizeof(ix), 1, fpOutput);
            }
        }

        /* 4.5: Global scale parameters */
        OUTPUT_PRECISION scale = (OUTPUT_PRECISION) mu[0];
        fwrite(&scale, sizeof(scale), 1, fpOutput);
        scale = (OUTPUT_PRECISION) sigma[0];
        fwrite(&scale, sizeof(scale), 1, fpOutput);
        scale = (OUTPUT_PRECISION) mu[1];
        fwrite(&scale, sizeof(scale), 1, fpOutput);
        scale = (OUTPUT_PRECISION) sigma[1];
        fwrite(&scale, sizeof(scale), 1, fpOutput);


        /* 5: relevance parameter log(sig_i) means and standard deviations */
        for (int i = 0; i < ali->nSites; i++) {
            OUTPUT_PRECISION h = (OUTPUT_PRECISION) wLambdaHi(lambdaMu, i);
            fwrite(&h, sizeof(h), 1, fpOutput);
        }
        for (int i = 0; i < ali->nSites; i++) {
            OUTPUT_PRECISION h = (OUTPUT_PRECISION) wLambdaHi(lambdaSig, i);
            fwrite(&h, sizeof(h), 1, fpOutput);
        }

        /* 6: relevance parameter log(sig_ij) means and standard deviations */
        for (int i = 0; i < ali->nSites - 1; i++)
            for (int j = i + 1; j < ali->nSites; j++) {       
                OUTPUT_PRECISION f =
                            (OUTPUT_PRECISION) wLambdaEij(lambdaMu, i, j);
                fwrite(&f, sizeof(f), 1, fpOutput);
            }
        for (int i = 0; i < ali->nSites - 1; i++)
            for (int j = i + 1; j < ali->nSites; j++) {
                OUTPUT_PRECISION f =
                            (OUTPUT_PRECISION) wLambdaEij(lambdaSig, i, j);
                fwrite(&f, sizeof(f), 1, fpOutput);
            }

        /* 7: sitewise marginals fi */
        for (int i = 0; i < ali->nSites; i++)
            for (int ai = 0; ai < ali->nCodes; ai++) {
                OUTPUT_PRECISION f = (OUTPUT_PRECISION) fi(i, ai);
                fwrite(&f, sizeof(f), 1, fpOutput);
            }

        /* 8: sitewise hi means and standard deviations */
        for (int i = 0; i < ali->nSites; i++)
            for (int ai = 0; ai < ali->nCodes; ai++) {
                OUTPUT_PRECISION h = (OUTPUT_PRECISION) wHi(xMu, i, ai);
                fwrite(&h, sizeof(h), 1, fpOutput);
            }
        for (int i = 0; i < ali->nSites; i++)
            for (int ai = 0; ai < ali->nCodes; ai++) {
                OUTPUT_PRECISION h = (OUTPUT_PRECISION) wHi(xSig, i, ai);
                fwrite(&h, sizeof(h), 1, fpOutput);
            }

        /* 9: coupling marginals & parameters */
        for (int i = 0; i < ali->nSites - 1; i++)
            for (int j = i + 1; j < ali->nSites; j++) {
                /* 9a: i, j dimensions */
                int ix = i + 1;
                int jx = j + 1;
                fwrite(&ix, sizeof(ix), 1, fpOutput);
                fwrite(&jx, sizeof(jx), 1, fpOutput);

                /* 9b: fij */
                for (int ai = 0; ai < ali->nCodes; ai++)
                    for (int aj = 0; aj < ali->nCodes; aj++) {
                        OUTPUT_PRECISION f =
                            (OUTPUT_PRECISION) fij(i, j, ai, aj);
                        fwrite(&f, sizeof(f), 1, fpOutput);
                    }

                /* 9c: pairwise eij means and standard deviations */
                for (int ai = 0; ai < ali->nCodes; ai++)
                    for (int aj = 0; aj < ali->nCodes; aj++) {
                        OUTPUT_PRECISION e =
                            (OUTPUT_PRECISION) wEij(xMu, i, j, ai, aj);
                        fwrite(&e, sizeof(e), 1, fpOutput);
                    }
                for (int ai = 0; ai < ali->nCodes; ai++)
                    for (int aj = 0; aj < ali->nCodes; aj++) {
                        OUTPUT_PRECISION e =
                            (OUTPUT_PRECISION) wEij(xSig, i, j, ai, aj);
                        fwrite(&e, sizeof(e), 1, fpOutput);
                    }
            }

        fclose(fpOutput);
    } else {
        fprintf(stderr, "Error writing parameters\n");
        exit(1);
    }
}

void OutputParametersFullPLMDCA(char *outputFile, const numeric_t *x,
    alignment_t *ali, options_t *options) {
    FILE *fpOutput = NULL;
    fpOutput = fopen(outputFile, "w");
    if (fpOutput != NULL) {
        /* 1: nSites */
        fwrite(&(ali->nSites), sizeof(ali->nSites), 1, fpOutput);

        /* 2: (Focus mode only) target sequence */
        if (ali->target >= 0) {
            for (int i = 0; i < ali->nSites; i++) {
                char c = (char) ali->alphabet[seq(ali->target, i)];
                fwrite(&c, sizeof(char), 1, fpOutput);
            }
        } else {
            char c = ali->alphabet[0];
            for (int i = 0; i < ali->nSites; i++)
                fwrite(&c, sizeof(c), 1, fpOutput);
        }

        /* 3: (Focus mode only) offset map */
        if (ali->target >= 0) {
            for (int i = 0; i < ali->nSites; i++) {
                int ix = ali->offsets[i];
                fwrite(&ix, sizeof(ix), 1, fpOutput);
            }
        } else {
            for (int i = 0; i < ali->nSites; i++) {
                int ix = i + 1;
                fwrite(&ix, sizeof(ix), 1, fpOutput);
            }
        }

        if (options->estimatorMAP == INFER_MAP_PLM_GAPREDUCE) {

            /* 4,5: sitewise marginals fi, twice */
            for (int x = 0; x < 2; x++)
                for (int i = 0; i < ali->nSites; i++)
                    for (int ai = -1; ai < ali->nCodes; ai++) {
                        double f = 0;
                        if (ai >= 0) f = (double) fi(i, ai);
                        fwrite(&f, sizeof(f), 1, fpOutput);
                    }

            /* 6: sitewise parameters hi */
            for (int i = 0; i < ali->nSites; i++)
                for (int ai = -1; ai < ali->nCodes; ai++) {
                    double h = 0;
                    if (ai >= 0) h = (double) xHi(i, ai);
                    fwrite(&h, sizeof(h), 1, fpOutput);
                }

            /* 7: coupling marginals & parameters fij, hi~, hj~, eij */
            for (int i = 0; i < ali->nSites - 1; i++)
                for (int j = i + 1; j < ali->nSites; j++) {
                    /* 7a: i, j dimensions */
                    int ix = i + 1;
                    int jx = j + 1;
                    fwrite(&ix, sizeof(ix), 1, fpOutput);
                    fwrite(&jx, sizeof(jx), 1, fpOutput);

                    /* 7b: fij */
                    for (int ai = -1; ai < ali->nCodes; ai++) {
                        for (int aj = -1; aj < ali->nCodes; aj++) {
                            double f = 0;
                            if ((ai >= 0) && (aj >= 0))
                                f = (double) fij(i, j, ai, aj);
                            fwrite(&f, sizeof(f), 1, fpOutput);
                        }
                    }

                    /* 7c,d: hi~, hj~ */
                    double h = 0;
                    for (int ai = -1; ai < ali->nCodes; ai++)
                        fwrite(&h, sizeof(h), 1, fpOutput);
                    for (int aj = -1; aj < ali->nCodes; aj++)
                        fwrite(&h, sizeof(h), 1, fpOutput);

                    /* 7e: eij */
                    for (int ai = -1; ai < ali->nCodes; ai++)
                        for (int aj = -1; aj < ali->nCodes; aj++) {
                            double e = 0;
                            if ((ai >= 0) && (aj >= 0))
                                e = (double) xEij(i, j, ai, aj);
                            fwrite(&e, sizeof(e), 1, fpOutput);
                        }
                }
        } else { /* Include gap characters */
            /* 4,5: sitewise marginals fi, twice */
            for (int x = 0; x < 2; x++)
                for (int i = 0; i < ali->nSites; i++)
                    for (int ai = 0; ai < ali->nCodes; ai++) {
                        double f = (double) fi(i, ai);
                        fwrite(&f, sizeof(f), 1, fpOutput);
                    }

            /* 6: sitewise parameters hi */
            for (int i = 0; i < ali->nSites; i++)
                for (int ai = 0; ai < ali->nCodes; ai++) {
                    double h = (double) xHi(i, ai);
                    fwrite(&h, sizeof(h), 1, fpOutput);
                }

            /* 7: coupling marginals & parameters fij, hi~, hj~, eij */
            for (int i = 0; i < ali->nSites - 1; i++)
                for (int j = i + 1; j < ali->nSites; j++) {
                    /* 7a: i, j dimensions */
                    int ix = i + 1;
                    int jx = j + 1;
                    fwrite(&ix, sizeof(ix), 1, fpOutput);
                    fwrite(&jx, sizeof(jx), 1, fpOutput);

                    /* 7b: fij */
                    for (int ai = 0; ai < ali->nCodes; ai++)
                        for (int aj = 0; aj < ali->nCodes; aj++) {
                            double f =
                                (double) fij(i, j, ai, aj);
                            fwrite(&f, sizeof(f), 1, fpOutput);
                        }

                    /* 7c,d: hi~, hj~ */
                    double h = 0;
                    for (int ai = 0; ai < ali->nCodes; ai++)
                        fwrite(&h, sizeof(h), 1, fpOutput);
                    for (int aj = 0; aj < ali->nCodes; aj++)
                        fwrite(&h, sizeof(h), 1, fpOutput);

                    /* 7e: eij */
                    for (int ai = 0; ai < ali->nCodes; ai++)
                        for (int aj = 0; aj < ali->nCodes; aj++) {
                            double e =
                                (double) xEij(i, j, ai, aj);
                            fwrite(&e, sizeof(e), 1, fpOutput);
                        }
                }
        }

        fclose(fpOutput);
    } else {
        fprintf(stderr, "Error writing parameters\n");
        exit(1);
    }
}

void OutputParametersFit(char *outputFile, const numeric_t *x,
    alignment_t *ali) {
    FILE *fpOutput = NULL;
    fpOutput = fopen(outputFile, "w");
    if (fpOutput != NULL) {
        /* Output values */
        fwrite(&(ali->nSeqs), sizeof(int), 1, fpOutput);
        fwrite(&(ali->nSites), sizeof(int), 1, fpOutput);
        fwrite(&(ali->nCodes), sizeof(int), 1, fpOutput);

        for (int i = 0; i < ali->nSites; i++)
            for (int ai = 0; ai < ali->nCodes; ai++) {
                OUTPUT_PRECISION h = (OUTPUT_PRECISION) xHi(i, ai);
                fwrite(&h, sizeof(h), 1, fpOutput);
            }

        for (int i = 0; i < ali->nSites - 1; i++)
            for (int j = i + 1; j < ali->nSites; j++)
                for (int ai = 0; ai < ali->nCodes; ai++)
                    for (int aj = 0; aj < ali->nCodes; aj++) {
                        OUTPUT_PRECISION h = (OUTPUT_PRECISION) xEij(i, j, ai, aj);
                        fwrite(&h, sizeof(OUTPUT_PRECISION), 1, fpOutput);
                    }

        fclose(fpOutput);
    } else {
        fprintf(stderr, "Error writing parameters\n");
        exit(1);
    }
}
#undef OUTPUT_PRECISION

void OutputCouplingScores(char *couplingsFile, const numeric_t *x,
    alignment_t *ali, options_t *options) {
    FILE *fpOutput = NULL;
    fpOutput = fopen(couplingsFile, "w");
    if (fpOutput != NULL) {
        /* Compute the norm of the coupling parameters between each pair */
        numeric_t *couplings =
        (numeric_t *) malloc((ali->nSites * (ali->nSites - 1) / 2)
                * sizeof(numeric_t));

        for (int i = 0; i < ali->nSites * (ali->nSites - 1) / 2;
            i++) couplings[i] = 0;
        for (int i = 0; i < ali->nSites - 1; i++)
            for (int j = i + 1; j < ali->nSites; j++) {
                /* Norm(eij) over ai, aj */
                numeric_t norm = 0.0;
                for (int ai = 0; ai < ali->nCodes; ai++)
                    for (int aj = 0; aj < ali->nCodes; aj++)
                        norm += xEij(i, j, ai, aj) * xEij(i, j, ai, aj);
                norm = sqrt(norm);
                coupling(i, j) = norm;
            }
        numeric_t nPairs =
            ((numeric_t) ((ali->nSites) * (ali->nSites - 1))) / 2.0;

        /* Remove first component of the norms (Average Product Correction) */
        if (!options->zeroAPC) {
            /* Determine the site-wise statistics of the norms */
            numeric_t C_avg = 0.0;
            numeric_t *C_pos_avg =
                (numeric_t *) malloc(ali->nSites * sizeof(numeric_t));
            for (int i = 0; i < ali->nSites; i++) {
                C_pos_avg[i] = 0.0;
            }
            for (int i = 0; i < ali->nSites - 1; i++) {
                for (int j = i + 1; j < ali->nSites; j++) {
                    C_pos_avg[i] +=
                        coupling(i, j) / (numeric_t) (ali->nSites - 1);
                    C_pos_avg[j] +=
                        coupling(i, j) / (numeric_t) (ali->nSites - 1);
                    C_avg += coupling(i, j) / nPairs;
                }
            }

            /* Remove the first component */
            for (int i = 0; i < ali->nSites - 1; i++)
                for (int j = i + 1; j < ali->nSites; j++)
                    coupling(i, j) =
                        coupling(i, j) - C_pos_avg[i] * C_pos_avg[j] / C_avg;    
        }

        /* Output scores */
        if (ali->target >= 0) {
            /* Focus mode */
            for (int i = 0; i < ali->nSites - 1; i++)
                for (int j = i + 1; j < ali->nSites; j++) {
                    char ai = (char) ali->alphabet[seq(ali->target, i)];
                    char aj = (char) ali->alphabet[seq(ali->target, j)];
                    fprintf(fpOutput, "%d %c %d %c 0 %f\n",
                        ali->offsets[i], ai, ali->offsets[j], aj,
                        coupling(i, j));
                }
        } else {
            for (int i = 0; i < ali->nSites - 1; i++)
                for (int j = i + 1; j < ali->nSites; j++)
                    fprintf(fpOutput, "%d - %d - 0 %f\n", i + 1, j + 1,
                        coupling(i, j));
        }

        fclose(fpOutput);
    } else {
        fprintf(stderr, "Error writing coupling scores\n");
        exit(1);
    }
}

numeric_t *DEBUGParams(alignment_t *ali) {
    /* Initialize parameters with dummy parameters for test I/O */
    ali->nParams = ali->nSites * ali->nCodes
        + ali->nSites * (ali->nSites - 1) / 2 * ali->nCodes * ali->nCodes;
    numeric_t *x = (numeric_t *) malloc(ali->nParams * sizeof(numeric_t));
    if (x == NULL) {
        fprintf(stderr,
            "ERROR: Failed to allocate a memory block for variables.\n");
        exit(1);
    }

    for (int i = 0; i < ali->nSites; i++)
            for (int ai = 0; ai < ali->nCodes; ai++)
                xHi(i, ai) = ((numeric_t) i + 1)
                           + ((numeric_t) ai)
                               / ((numeric_t) ali->nCodes);
    for (int i = 0; i < ali->nSites - 1; i++)
            for (int j = i + 1; j < ali->nSites; j++)
                for (int ai = 0; ai < ali->nCodes; ai++)
                    for (int aj = 0; aj < ali->nCodes; aj++)
                        xEij(i, j, ai, aj) = ((numeric_t) 1000 * (i + 1))
                                           + ((numeric_t) (j + 1))
                                           + ((numeric_t) ai)
                                              /((numeric_t) ali->nCodes)
                                           + ((numeric_t) aj) / 100
                                              /((numeric_t) ali->nCodes);
    return x;
}