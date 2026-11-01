#include "ns3/core-module.h"
#include "ns3/tcp-hybrid-cubic.h"

#include <array>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <vector>

using namespace ns3;

int
main(int argc, char* argv[])
{
    bool v2 = true;
    CommandLine command;
    command.AddValue("v2", "Evaluate the scale-normalized v2-A model", v2);
    command.Parse(argc, argv);

    // Socket creation forks the configured congestion-control prototype.  Keep
    // this check beside inference parity so local attribute-copy regressions
    // fail the verification command instead of silently enabling large traces.
    Ptr<TcpMlCubic> prototype = CreateObject<TcpMlCubic>();
    prototype->SetAttribute("GrowthTraceEnabled", BooleanValue(false));
    Ptr<TcpCongestionOps> forked = prototype->Fork();
    BooleanValue growthTraceEnabled;
    forked->GetAttribute("GrowthTraceEnabled", growthTraceEnabled);
    if (growthTraceEnabled.Get())
    {
        std::cerr << "GrowthTraceEnabled was not preserved by Fork()\n";
        return 1;
    }

    const std::vector<std::array<float, 7>> v1Vectors = {
        {{1.24959505f,
          206.301239f,
          0.000731220469f,
          1.19452667f,
          0.0121993264f,
          19.0030937f,
          18.9330406f}},
        {{1.0f, 100.0f, 0.0f, 1.05f, 0.001f, 10.0f, 8.0f}},
        {{1.8f, 350.0f, 0.005f, 1.6f, 0.05f, 45.0f, 40.0f}},
        {{0.2f, -10.0f, -0.1f, 0.5f, -0.1f, 1.0f, 1.0f}},
        {{6.0f, 1000.0f, 0.1f, 4.0f, 0.5f, 200.0f, 200.0f}},
    };
    const std::vector<std::array<float, 7>> v2Vectors = {
        {{1.0f, 0.95f, 0.0f, 1.1f, 0.001f, 1.0f, 0.9f}},
        {{1.2f, 0.65f, 0.01f, 1.5f, 0.02f, 2.0f, 1.0f}},
        {{0.8f, 0.30f, -0.02f, 1.0f, 0.05f, 0.5f, 0.4f}},
        {{0.1f, -1.0f, -0.5f, 0.2f, -0.1f, 0.0f, 0.0f}},
        {{5.0f, 3.0f, 0.5f, 5.0f, 1.0f, 20.0f, 2.0f}},
    };
    const std::array<float, 5> v1Expected = {
        0.001453124f, 0.003637592f, 0.800258636f, 0.003184949f, 0.647191346f};
    const std::array<float, 5> v2Expected = {
        0.000570779f, 0.061334565f, 0.000002418f, 0.000003667f, 0.999870539f};
    const auto& vectors = v2 ? v2Vectors : v1Vectors;
    const auto& expected = v2 ? v2Expected : v1Expected;

    std::cout << "vector_id,p_congestion\n";
    std::cout << std::fixed << std::setprecision(9);
    for (std::size_t i = 0; i < vectors.size(); ++i)
    {
        const float probability =
            v2 ? TcpMlCubic::PredictV2CongestionForFeatures(vectors[i])
               : TcpMlCubic::PredictCongestionForFeatures(vectors[i]);
        std::cout << i << ',' << probability << '\n';
        if (std::fabs(probability - expected[i]) > 0.0000005f)
        {
            std::cerr << "Inference parity failed for vector " << i << "\n";
            return 1;
        }
    }
    return 0;
}
