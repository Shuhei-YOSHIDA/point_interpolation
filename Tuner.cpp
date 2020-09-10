#include <iostream>
#include <vector>
#include <chrono>

#include <Open3D/Open3D.h>
#include <opencv2/opencv.hpp>
#include <time.h>

#include "models/envParams.cpp"
#include "data/loadParams.cpp"
#include "preprocess/grid_pcd.cpp"
#include "methods/linear.cpp"
#include "methods/mrf.cpp"
#include "methods/pwas.cpp"
#include "methods/original.cpp"
#include "postprocess/evaluate.cpp"
#include "postprocess/restore_pcd.cpp"

using namespace std;
using namespace open3d;

ofstream ofs;

double tune(EnvParams envParams)
{
    string img_path = envParams.folder_path + to_string(data_no);
    if (envParams.isRGB)
    {
        img_path += "_rgb.png";
    }
    else
    {
        img_path += ".png";
    }
    const string pcd_path = envParams.folder_path + to_string(data_no) + ".pcd";

    auto img = cv::imread(img_path);
    cv::Mat blured;
    cv::GaussianBlur(img, blured, cv::Size(3, 3), 0.5);

    geometry::PointCloud pointcloud;
    auto pcd_ptr = make_shared<geometry::PointCloud>();
    if (!io::ReadPointCloud(pcd_path, pointcloud))
    {
        cout << "Cannot read" << endl;
    }

    auto start = chrono::system_clock::now();

    vector<vector<double>> original_grid, filtered_grid, original_interpolate_grid, filtered_interpolate_grid;
    vector<vector<int>> target_vs, base_vs;
    *pcd_ptr = pointcloud;
    int layer_cnt = 16;
    calc_grid(pcd_ptr, envParams, original_grid, filtered_grid, original_interpolate_grid, filtered_interpolate_grid, target_vs, base_vs, layer_cnt);

    vector<vector<int>> original_vs;
    if (envParams.isFullHeight)
    {
        original_vs = vector<vector<int>>(envParams.height, vector<int>(envParams.width, 0));
        for (int i = 0; i < envParams.height; i++)
        {
            for (int j = 0; j < envParams.width; j++)
            {
                original_vs[i][j] = i;
            }
        }
        swap(original_vs, target_vs);
        cout << original_vs.size() << endl;
        cout << target_vs.size() << endl;
    }
    else
    {
        original_vs = target_vs;
    }

    vector<vector<double>> interpolated_z;
    if (envParams.method == "linear")
    {
        linear(interpolated_z, filtered_interpolate_grid, target_vs, base_vs, envParams);
    }
    if (envParams.method == "mrf")
    {
        mrf(interpolated_z, filtered_interpolate_grid, target_vs, base_vs, envParams, blured);
    }
    if (envParams.method == "pwas")
    {
        pwas(interpolated_z, filtered_interpolate_grid, target_vs, base_vs, envParams, blured);
        cout << "aaa" << endl;
    }
    if (envParams.method == "original")
    {
        original(interpolated_z, filtered_interpolate_grid, target_vs, base_vs, envParams, blured);
    }

    {
        cv::Mat interpolate_img = cv::Mat::zeros(target_vs.size(), envParams.width, CV_8UC1);
        for (int i = 0; i < target_vs.size(); i++)
        {
            for (int j = 0; j < envParams.width; j++)
            {
                if (interpolated_z[i][j] > 0)
                {
                    interpolate_img.at<uchar>(i, j) = 255;
                }
            }
        }
        //cv::imshow("hoge", interpolate_img);
        //cv::waitKey();
    }

    double error = 0;
    { // Evaluate
        double tim = chrono::duration_cast<chrono::milliseconds>(chrono::system_clock::now() - start).count();
        double ssim, mse, mre;
        evaluate(interpolated_z, original_grid, target_vs, original_vs, envParams, layer_cnt, ssim, mse, mre);

        cout << tim << "ms" << endl;
        cout << "SSIM = " << fixed << setprecision(5) << ssim << endl;
        cout << "MSE = " << mse << endl;
        cout << "MRE = " << mre << endl;
        ofs << data_no << "," << tim << "," << ssim << "," << mse << "," << mre << "," << endl;

        error = mre;
    }

    auto interpolated_ptr = make_shared<geometry::PointCloud>();
    auto original_ptr = make_shared<geometry::PointCloud>();
    restore_pcd(interpolated_z, original_grid, target_vs, original_vs, envParams, blured, interpolated_ptr, original_ptr);

    if (see_res)
    {
        visualization::DrawGeometries({original_ptr, interpolated_ptr}, "Original", 1600, 900);
    }
    if (!io::WritePointCloudToPCD(envParams.folder_path + to_string(data_no) + "_linear.pcd", *interpolated_ptr))
    {
        cout << "Cannot write" << endl;
    }

    return error;
}

int main(int argc, char *argv[])
{
    EnvParams params_use = loadParams("miyanosawa_0204_rgb_original");
    ofs = ofstream(params_use.of_name);

    for (int i = 0; i < params_use.data_ids.size(); i++)
    {
        segmentate(params_use.data_ids[i], params_use, true);
    }
    return 0;

    //params_use = params_miyanosawa_3_3_pwas_champ;
    double best_error = 1000000;
    double best_sigma_c = 1;
    double best_sigma_s = 1;
    double best_sigma_r = 1;
    int best_r = 1;
    // best params 2020/08/03 sigma_c:1000 sigma_s:1.99 sigma_r:19 r:7
    // best params 2020/08/10 sigma_c:12000 sigma_s:1.6 sigma_r:19 r:7
    // best params 2020/08/10 sigma_c:8000 sigma_s:1.6 sigma_r:19 r:7

    for (double sigma_c = 10; sigma_c <= 1000; sigma_c += 10)
    {
        for (double sigma_s = 0.1; sigma_s < 1.7; sigma_s += 0.1)
        {
            for (double sigma_r = 1; sigma_r < 100; sigma_r += 10)
            {
                for (int r = 1; r < 9; r += 2)
                {
                    double error = 0;
                    for (int i = 0; i < params_use.data_ids.size(); i++)
                    {
                        error += segmentate(params_use.data_ids[i], params_use);
                    }

                    if (best_error > error)
                    {
                        best_error = error;
                        best_sigma_c = sigma_c;
                        best_sigma_s = sigma_s;
                        best_sigma_r = sigma_r;
                        best_r = r;
                    }
                }
            }
        }
    }

    cout << "Sigma C = " << best_sigma_c << endl;
    cout << "Sigma S = " << best_sigma_s << endl;
    cout << "Sigma R = " << best_sigma_r << endl;
    cout << "R = " << best_r << endl;
    cout << "Mean error = " << best_error / params_use.data_ids.size() << endl;
}