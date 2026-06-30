struct FixedDataset {
    bool ready = false;

    std::vector<std::string> server_ids;
    std::vector<std::vector<double>> server_imgs;

    std::vector<std::vector<double>> client_imgs;
    std::vector<bool> client_is_close;
};
FixedDataset fixed_ds;
static FixedDataset build_fixed_dataset_once(
    const std::string& server_path,
    const std::string& client_path,
    int server_size,
    int client_size ,
    std::mt19937& rng 
) {

    (void)client_path;

    FixedDataset ds;

    load_server_from_json(server_path, server_size, rng, ds.server_ids, ds.server_imgs);
    load_client_from_json_for_server(
        ds.server_ids,
        ds.server_imgs,
        client_size,
        rng,
        ds.client_imgs,
        ds.client_is_close
    );
     
    /*overwrite_client_with_conditioned_noise(
        ds.client_imgs, ds. client_is_close,
        rng,
        CLOSE_NOISE_START, CLOSE_NOISE_END,
        FAR_NOISE_START, FAR_NOISE_END
    );*/

    ds.ready = true;
    return ds;
}


