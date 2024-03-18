#include <iostream>
#include <cmath>

#include <torch/torch.h>
#ifdef USE_HIP
#include <hip/hip_runtime.h>
#elif defined(USE_CUDA)
#include <torch/cuda.h>
#endif

#include <opencv2/core/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "vendor/gsplat/config.h"
#include "project_gaussians.hpp"
#include "rasterize_gaussians.hpp"
#include "constants.hpp"
#include "cv_utils.hpp"

using namespace torch::indexing;



int main(int argc, char **argv){
    int width = 256,
        height = 256;
    int numPoints = 32; //100000;
    int iterations = 1000;
    float learningRate = 0.01;

    torch::Device device = torch::kCPU;
    if (torch::cuda::is_available() && !(argc == 2 && std::string(argv[1]) == "--cpu")){
        std::cout << "Using CUDA" << std::endl;
        device = torch::kCUDA;
    }else{
        std::cout << "Using CPU" << std::endl;
    }

    // Test image
    // Top left red
    // Bottom right blue
    torch::Tensor gtImage = torch::ones({height, width, 3});
    gtImage.index_put_({Slice(None, height / 2), Slice(None, width / 2), Slice()}, torch::tensor({1.0, 0.0, 0.0}));
    gtImage.index_put_({Slice(height / 2, None), Slice(width / 2, None), Slice()}, torch::tensor({0.0, 0.0, 1.0}));

    // cv::Mat image = tensorToImage(gtImage);
    // cv::cvtColor(image, image, cv::COLOR_RGB2BGR);
    // cv::imwrite("test.png", image);

    gtImage = gtImage.to(device);
    double fovX = PI / 2.0; // horizontal field of view (90 deg)
    double focal = 0.5 * static_cast<double>(width) / std::tan(0.5 * fovX);

    TileBounds tileBounds = std::make_tuple((width + BLOCK_X - 1) / BLOCK_X,
                      (height + BLOCK_Y - 1) / BLOCK_Y,
                      1);
    
    // torch::Tensor imgSize = torch::tensor({width, height, 1}, device);
    // torch::Tensor block = torch::tensor({BLOCK_X, BLOCK_Y, 1}, device);
    
    // Init gaussians
#ifdef USE_CUDA
    torch::cuda::manual_seed_all(0);
#endif
    torch::manual_seed(0);

    // TODO: remove
    // Random points, scales and colors
    torch::Tensor means = 2.0 * (torch::rand({numPoints, 3}, torch::kCPU) - 0.5); // Positions [-1, 1]
    torch::Tensor scales = torch::rand({numPoints, 3}, torch::kCPU);
    torch::Tensor rgbs = torch::rand({numPoints, 3}, torch::kCPU);
    
    // Random rotations (quaternions)
    // quats = ( sqrt(1-u) sin(2πv), sqrt(1-u) cos(2πv), sqrt(u) sin(2πw), sqrt(u) cos(2πw))
    torch::Tensor u = torch::rand({numPoints, 1}, torch::kCPU);
    torch::Tensor v = torch::rand({numPoints, 1}, torch::kCPU);
    torch::Tensor w = torch::rand({numPoints, 1}, torch::kCPU);

    means = means.to(device);
    scales = scales.to(device);
    rgbs = rgbs.to(device);
    u = u.to(device);
    v = v.to(device);
    w = w.to(device);    

    // TODO: uncomment
    // // Random points, scales and colors
    // torch::Tensor means = 2.0 * (torch::rand({numPoints, 3}, device) - 0.5); // Positions [-1, 1]
    // torch::Tensor scales = torch::rand({numPoints, 3}, device);
    // torch::Tensor rgbs = torch::rand({numPoints, 3}, device);
    
    // // Random rotations (quaternions)
    // // quats = ( sqrt(1-u) sin(2πv), sqrt(1-u) cos(2πv), sqrt(u) sin(2πw), sqrt(u) cos(2πw))
    // torch::Tensor u = torch::rand({numPoints, 1}, device);
    // torch::Tensor v = torch::rand({numPoints, 1}, device);
    // torch::Tensor w = torch::rand({numPoints, 1}, device);
    torch::Tensor quats = torch::cat({
                torch::sqrt(1.0 - u) * torch::sin(2.0 * PI * v),
                torch::sqrt(1.0 - u) * torch::cos(2.0 * PI * v),
                torch::sqrt(u) * torch::sin(2.0 * PI * w),
                torch::sqrt(u) * torch::cos(2.0 * PI * w),
            }, -1);
    
    torch::Tensor opacities = torch::ones({numPoints, 1}, device);

    // View matrix (translation in Z by 8 units)
    torch::Tensor viewMat = torch::tensor({
            {1.0, 0.0, 0.0, 0.0},
            {0.0, 1.0, 0.0, 0.0},
            {0.0, 0.0, 1.0, 8.0},
            {0.0, 0.0, 0.0, 1.0}
        }, device);

    torch::Tensor background = torch::zeros(gtImage.size(2), device);

    means.requires_grad_();
    scales.requires_grad_();
    quats.requires_grad_();
    rgbs.requires_grad_();
    opacities.requires_grad_();

    torch::optim::Adam optimizer({rgbs, means, scales, opacities, quats}, learningRate);
    torch::nn::MSELoss mseLoss;
    torch::Tensor outImg;

    for (size_t i = 0; i < iterations; i++){
        if (device == torch::kCPU){
            auto p = ProjectGaussiansCPU::Apply(means, scales, 1, 
                                quats, viewMat, viewMat,
                                focal, focal,
                                width / 2,
                                height / 2,
                                height,
                                width);

            outImg = RasterizeGaussiansCPU::apply(
                p[0], // xys
                p[1], // radii,
                p[2], // conics
                torch::sigmoid(rgbs),
                torch::sigmoid(opacities),
                p[3], // cov2d
                p[4], // camDepths
                height,
                width,
                background);
        }else{
            auto p = ProjectGaussians::apply(means, scales, 1, 
                                    quats, viewMat, viewMat,
                                    focal, focal,
                                    width / 2,
                                    height / 2,
                                    height,
                                    width,
                                    tileBounds);

            outImg = RasterizeGaussians::apply(
                p[0], // xys
                p[1], // depths
                p[2], // radii,
                p[3], // conics
                p[4], // numTilesHit
                torch::sigmoid(rgbs),
                torch::sigmoid(opacities),
                height,
                width,
                background);
        }

        outImg.requires_grad_();
        torch::Tensor loss = mseLoss(outImg, gtImage);
        optimizer.zero_grad();
        loss.backward();
        optimizer.step();

        std::cout << "Iteration " << std::to_string(i + 1) << "/" << std::to_string(iterations) << " Loss: " << loss.item<float>() << std::endl; 

        cv::Mat image = tensorToImage(outImg.detach().cpu());
        cv::cvtColor(image, image, cv::COLOR_RGB2BGR);
        cv::imwrite("render/" + std::to_string(i + 1) + ".png", image);
    }
}