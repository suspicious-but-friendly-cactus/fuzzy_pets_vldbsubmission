// main.cpp  (Option A: stop early when FP/FN targets are met)
//
// Assumes your project already provides these in utils_.h:
//   - load_server_from_json(...)
//   - load_client_from_json(...)
//
// And your headers:
//   - lsh_e2lsh.h  (class E2LSH with ctor (dim,L,k,w) and hash_values(vector<int>))
//   - bloom_filter.h (class BloomFilter with clear(), insert(img,lsh), membership(img,lsh), fill_ratio())

#include <iostream>
#include <vector>
#include <string>
#include <random>
#include <limits>

#include "lsh_e2lsh.h"
#include "bloom_filter.h"
#include "Utils/utils.h"
using namespace std;
using std::cout;
using std::string;
using std::vector;
using std::mt19937;

struct Result {
    int L = 0;
    int k = 0;
    double w = 0.0;
    double fp = 1.0;
    double fn = 1.0;
    double score = std::numeric_limits<double>::infinity();
    double fill = 0.0;
};

int main() {
    // =========================
    // Fixed experiment settings
    // =========================
    const int dim = 784;            // MNIST 28x28
    const int client_size = 10;        // as requested
    const int filter_size = 100000; // bloom bits
    const int NUM_RUNS = 5;         // increase for stability

    // How much you value FN vs FP in the score (lower is better)
    // alpha=0.7 means FN counts 70%, FP counts 30%.
    const double alpha = 0.7;

    // =========================
    // Early-stop targets (Option A)
    // =========================
    // Stop the grid search as soon as you find a config with BOTH:
    //   FP <= target_fp AND FN <= target_fn
    const double target_fp = 0.05;
    const double target_fn = 0.10;

    // Bloom saturation guard (skip configs that set too many bits)
    const double max_fill_ratio = 0.60;

    // =========================
    // Grid search space
    // =========================
    // Add more values here if you want a bigger search;
    // just keep in mind runtime grows ~ |L|*|k|*|w|*NUM_RUNS.
    vector<int> L_vals = {1,3,5,8, 10,12,18, 20,25,30, 40,50,55, 80, 160};
    vector<int> k_vals = {2, 3, 4, 5, 6, 8, 10, 12,15,20,25,30,40,45,60,80,100};
    vector<double> w_vals = {10,20,30,40,50,60, 80, 90,100, 120, 180, 200,400,500,1000,1500};


    // Your dataset sizes / paths
    const string server_path = "gowalla_server.json";
    const string client_path = "gowalla_client.json";
    const int server_N = 100; // adjust to match your setup / speed needs

    // Deterministic RNG
    mt19937 global_rng(1);

    Result best;

    cout << "Starting grid search...\n"
         << "Targets: FP <= " << target_fp << ", FN <= " << target_fn << "\n"
         << "Client size: " << client_size << ", Server N: " << server_N << "\n"
         << "Bloom size: " << filter_size << ", Max fill: " << max_fill_ratio << "\n\n";

         mt19937 run_rng(global_rng()); // different seed per run (repeatable overall)
        vector<string> server_ids;
        vector<vector<int>> server_imgs;

        load_server_from_json(
            server_path,
            server_N,
            run_rng,
            server_ids,
            server_imgs
        );

    BloomFilter bloom(filter_size);
    // =========================
    // Grid search
    // =========================
    for (int L : L_vals) {
        for (int k : k_vals) {
            for (double w : w_vals) {

                E2LSH lsh(dim, L, k, w);

                double fp_sum = 0.0;
                double fn_sum = 0.0;
                double fill_sum = 0.0;

                for (int run = 0; run < NUM_RUNS; ++run) {
                    mt19937 run_rng(global_rng()); // different seed per run (repeatable overall)

                    
                    //cout << "BLOOM BEFORE CLEAR"<< endl;
                    bloom.clear();
                    //cout << "BLOOM CLEARED" << endl;
                    for (const auto& img : server_imgs) {
                        bloom.insert(img, lsh);
                    }
                    //cout << "SERVER: BLOOM INSERTED" << endl;

                    fill_sum += bloom.fill_ratio();

                    vector<vector<int>> client_imgs;
                    vector<bool> client_is_close;
                    vector<string> client_relates_to;

                    load_client_from_json(
                        client_path,
                        server_ids,
                        client_size,
                        run_rng,
                        client_imgs,
                        client_is_close,
                        client_relates_to
                    );
                    // Test my code: force Client n/2 to be the same as server, the rest full noise
                        //overwrite_client_with_conditioned_noise(server_ids,server_imgs, client_imgs,client_relates_to,client_is_close,run_rng,
                        // 0, 30,        // close noise
                        //150, 200     // far noise
                        //);

                    //cout << "CLIENT LOADED " << endl;
                    int FP = 0, TN = 0, TP = 0, FN = 0;

                    for (size_t i = 0; i < client_imgs.size(); ++i) {
                        const bool is_close = client_is_close[i];
                        const bool reported = bloom.membership(client_imgs[i], lsh);
                          //cout << "COUNTING  CLIENT" << endl;
                        if (is_close) {
                            if (reported) TP++;
                            else FN++;
                        } else {
                            if (reported) FP++;
                            else TN++;
                        }
                    }
                    //cout << "BLOOM TESTED" << endl;
                    const double fp_rate = (FP + TN) ? double(FP) / double(FP + TN) : 0.0;
                    const double fn_rate = (TP + FN) ? double(FN) / double(TP + FN) : 0.0;

                    fp_sum += fp_rate;
                    fn_sum += fn_rate;
                }

                const double avg_fp = fp_sum / NUM_RUNS;
                const double avg_fn = fn_sum / NUM_RUNS;
                const double avg_fill = fill_sum / NUM_RUNS;

                // Skip configs that saturate Bloom
                if (avg_fill > max_fill_ratio) {
                    cout << "[skip(fill)] L=" << L << " k=" << k << " w=" << w
                         << " | fill=" << avg_fill << "\n";
                    continue;
                }

                const double score = alpha * avg_fn + (1.0 - alpha) * avg_fp;

                cout << "[grid] "
                     << "L=" << L
                     << " k=" << k
                     << " w=" << w
                     << " | fp=" << avg_fp
                     << " fn=" << avg_fn
                     << " score=" << score
                     << " fill=" << avg_fill
                     << "\n";

                if (score < best.score) {
                    best = {L, k, w, avg_fp, avg_fn, score, avg_fill};
                }

                // =========================
                // Option A: Stop early if targets met
                // =========================
                if (avg_fp <= target_fp && avg_fn <= target_fn) {
                    cout << "\nHit target thresholds! Stopping early.\n";
                    best = {L, k, w, avg_fp, avg_fn, score, avg_fill};
                    goto DONE;
                }
            }
        }
    }

DONE:
    cout << "\n=========================\n";
    cout << "BEST CONFIGURATION\n";
    cout << "=========================\n";
    cout << "L = " << best.L << "\n";
    cout << "k = " << best.k << "\n";
    cout << "w = " << best.w << "\n";
    cout << "FP = " << best.fp << "\n";
    cout << "FN = " << best.fn << "\n";
    cout << "Score = " << best.score << "\n";
    cout << "Bloom fill = " << best.fill << "\n";

    return 0;
}
