#include <math.h>
#include <vector>

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <xtensor/xio.hpp>

#include <mure/mure.hpp>
#include "coarsening.hpp"
#include "refinement.hpp"
#include "criteria.hpp"

#include <chrono>


/// Timer used in tic & toc
auto tic_timer = std::chrono::high_resolution_clock::now();

/// Launching the timer
void tic()
{
    tic_timer = std::chrono::high_resolution_clock::now();
}


/// Stopping the timer and returning the duration in seconds
double toc()
{
    const auto toc_timer = std::chrono::high_resolution_clock::now();
    const std::chrono::duration<double> time_span = toc_timer - tic_timer;
    return time_span.count();
}

double exact_solution(double x, double t)   {
    double u = 0;
    
    if (x >= -1 and x < t)
    {
        u = (1 + x) / (1 + t);
    }
    
    if (x >= t and x < 1)
    {
        u = (1 - x) / (1 - t);
    }

    return u;
}

template<class Config>
auto init_f(mure::Mesh<Config> &mesh, double t)
{
    constexpr std::size_t nvel = 2;
    mure::BC<1> bc{ {{ {mure::BCType::dirichlet, 0},
                       {mure::BCType::dirichlet, 0},
                    }} };

    mure::Field<Config, double, nvel> f("f", mesh, bc);
    f.array().fill(0);

    mesh.for_each_cell([&](auto &cell) {
        auto center = cell.center();
        auto x = center[0];
        double u = 0;

        if (x >= -1 and x < t)
        {
            u = (1 + x) / (1 + t);
        }
        if (x >= t and x < 1)
        {
            u = (1 - x) / (1 - t);
        }

        //double u = exp(-20.0 * x * x);

        //double v = .5 * u; 
        double v = .5 * u * u;

        f[cell][0] = .5 * (u + v);
        f[cell][1] = .5 * (u - v);
    });

    return f;
}

template<class Field, class interval_t, class FieldTag>
xt::xtensor<double, 1> prediction(const Field& f, std::size_t level_g, std::size_t level, const interval_t &i, const std::size_t item, 
                                  const FieldTag & tag, std::map<std::tuple<std::size_t, std::size_t, std::size_t, interval_t>, 
                                  xt::xtensor<double, 1>> & mem_map)
{

    // We check if the element is already in the map
    auto it = mem_map.find({item, level_g, level, i});
    if (it != mem_map.end())   {
        return it->second;
    }
    else {

        auto mesh = f.mesh();
        xt::xtensor<double, 1> out = xt::empty<double>({i.size()/i.step});//xt::eval(f(item, level_g, i));
        auto mask = mesh.exists(level_g + level, i);

        // std::cout << level_g + level << " " << i << " " << mask << "\n"; 
        if (xt::all(mask))
        {         
            return xt::eval(f(item, level_g + level, i));
        }

        auto step = i.step;
        auto ig = i / 2;
        ig.step = step >> 1;
        xt::xtensor<double, 1> d = xt::empty<double>({i.size()/i.step});

        for (int ii=i.start, iii=0; ii<i.end; ii+=i.step, ++iii)
        {
            d[iii] = (ii & 1)? -1.: 1.;
        }

    
        auto val = xt::eval(prediction(f, level_g, level-1, ig, item, tag, mem_map) - 1./8 * d * (prediction(f, level_g, level-1, ig+1, item, tag, mem_map) 
                                                                                       - prediction(f, level_g, level-1, ig-1, item, tag, mem_map)));
        

        xt::masked_view(out, !mask) = xt::masked_view(val, !mask);
        for(int i_mask=0, i_int=i.start; i_int<i.end; ++i_mask, i_int+=i.step)
        {
            if (mask[i_mask])
            {
                out[i_mask] = f(item, level_g + level, {i_int, i_int + 1})[0];
            }
        }

        // The value should be added to the memoization map before returning
        return mem_map[{item, level_g, level, i}] = out;

        //return out;
    }

}


template<class Field, class interval_t>
xt::xtensor<double, 2> prediction_all(const Field& f, std::size_t level_g, std::size_t level, const interval_t &i, 
                                  std::map<std::tuple<std::size_t, std::size_t, interval_t>, 
                                  xt::xtensor<double, 2>> & mem_map)
{

    using namespace xt::placeholders;
    // We check if the element is already in the map
    auto it = mem_map.find({level_g, level, i});
    if (it != mem_map.end())
    {
        return it->second;
    }
    else
    {
        auto mesh = f.mesh();
        std::vector<std::size_t> shape = {i.size(), 2};
        xt::xtensor<double, 2> out = xt::empty<double>(shape);
        auto mask = mesh.exists(level_g + level, i);

        xt::xtensor<double, 2> mask_all = xt::empty<double>(shape);
        xt::view(mask_all, xt::all(), 0) = mask;
        xt::view(mask_all, xt::all(), 1) = mask;

        // std::cout << level_g + level << " " << i << " " << mask << "\n"; 
        if (xt::all(mask))
        {         
            return xt::eval(f(level_g + level, i));
        }

        auto ig = i / 2;
        ig.step = 1;

        xt::xtensor<double, 2> val = xt::empty<double>(shape);
        xt::view(val, xt::range(0, _, 2)) = xt::eval(prediction_all(f, level_g, level-1, ig, mem_map) - 1./8 * (prediction_all(f, level_g, level-1, ig+1, mem_map) 
                                                                                                                    - prediction_all(f, level_g, level-1, ig-1, mem_map)));
        xt::view(val, xt::range(1, _, 2)) = xt::eval(prediction_all(f, level_g, level-1, ig, mem_map) + 1./8 * (prediction_all(f, level_g, level-1, ig+1, mem_map) 
                                                                                                                    - prediction_all(f, level_g, level-1, ig-1, mem_map)));
        std::cout << "\n\n" << level_g << " " << level << " " << i << "\n\n";
        std::cout << "\n\n" << xt::adapt(mask.shape()) << " " << xt::adapt(out.shape()) << " " << xt::adapt(val.shape()) << "\n\n";
        xt::masked_view(out, !mask_all) = xt::masked_view(val, !mask_all);
        for(int i_mask=0, i_int=i.start; i_int<i.end; ++i_mask, ++i_int)
        {
            if (mask[i_mask])
            {
                xt::view(out, i_mask) = f(level_g + level, {i_int, i_int + 1})[0];
            }
        }

        // The value should be added to the memoization map before returning
        return mem_map[{level_g, level, i}] = out;
    }
}

template<class Field, class FieldTag>
void one_time_step(Field &f, const FieldTag & tag)
{
    constexpr std::size_t nvel = Field::size;
    double lambda = 1., s = 1.0;
    auto mesh = f.mesh();
    auto max_level = mesh.max_level();

    mure::mr_projection(f);
    mure::mr_prediction(f);


    // MEMOIZATION
    // All is ready to do a little bit  of mem...
    using interval_t = typename Field::Config::interval_t;
    std::map<std::tuple<std::size_t, std::size_t, std::size_t, interval_t>, xt::xtensor<double, 1>> memoization_map;
    memoization_map.clear(); // Just to be sure...

    Field new_f{"new_f", mesh};
    new_f.array().fill(0.);

    for (std::size_t level = 0; level <= max_level; ++level)
    {
        auto exp = mure::intersection(mesh[mure::MeshType::cells][level],
                                      mesh[mure::MeshType::cells][level]);
        exp([&](auto, auto &interval, auto) {
            auto i = interval[0];



            std::size_t j = max_level - level;
            // auto tmp = i*(1<<j);
            // std::cout<<std::endl<<"Level "<<level<<" interval "<<i<<" with step"<<i.step<<" transformed "<<i*(1<<j)<<" with step "<<tmp.step;

            double coeff = 1. / (1 << j);
            auto fp = f(0, level, i) + coeff * (prediction(f, level, j, i*(1<<j)-1, 0, tag, memoization_map)
                                             -  prediction(f, level, j, (i+1)*(1<<j)-1, 0, tag, memoization_map));

            // std::cout << "calcul fm\n";
            auto fm = f(1, level, i) - coeff * (prediction(f, level, j, i*(1<<j), 1, tag, memoization_map)
                                             -  prediction(f, level, j, (i+1)*(1<<j), 1, tag, memoization_map));

            auto uu = xt::eval(fp + fm);
            auto vv = xt::eval(lambda * (fp - fm));

            vv = (1 - s) * vv + s * .5 * uu * uu;
            //vv = (1 - s) * vv + s * .5 * uu;

            new_f(0, level, i) = .5 * (uu + 1. / lambda * vv);
            new_f(1, level, i) = .5 * (uu - 1. / lambda * vv);
        });
    }

    std::swap(f.array(), new_f.array());
}

template<class Field>
void save_solution(Field &f, double eps, std::size_t ite, std::string ext)
{
    using Config = typename Field::Config;
    auto mesh = f.mesh();
    std::size_t min_level = mesh.min_level();
    std::size_t max_level = mesh.max_level();

    std::stringstream str;
    str << "LBM_D1Q2_Burgers_" << ext << "_lmin_" << min_level << "_lmax-" << max_level << "_eps-"
        << eps << "_ite-" << ite;

    auto h5file = mure::Hdf5(str.str().data());
    h5file.add_mesh(mesh);
    mure::Field<Config> level_{"level", mesh};
    mure::Field<Config> u{"u", mesh};
    mesh.for_each_cell([&](auto &cell) {
        level_[cell] = static_cast<double>(cell.level);
        u[cell] = f[cell][0] + f[cell][1];
    });
    h5file.add_field(u);
    h5file.add_field(f);
    h5file.add_field(level_);
}

// template<class Field, class FieldTag>
// double compute_error(const Field & f, const FieldTag & tag, double t)
// {
//     double error_to_return = 0.0;

//     // Getting ready for memoization
//     using interval_t = typename Field::Config::interval_t;
//     std::map<std::tuple<std::size_t, std::size_t, std::size_t, interval_t>, xt::xtensor<double, 1>> memoization_map;
//     memoization_map.clear();


//     auto mesh = f.mesh();
//     auto max_level = mesh.max_level();

//     double dx = 1 << max_level;


//     Field error_cell_by_cell{"error_cell_by_cell", mesh};
//     error_cell_by_cell.array().fill(0.);


//     auto subset = intersection(mesh.initial_mesh(), mesh.initial_mesh()).on(max_level);
//     subset([&](auto, auto &interval, auto) {
//         auto i = interval[0];


//         std::cout<<"\n\nHere  "<<i;

//         auto fp = prediction(f, max_level, 0, i, 0, tag, memoization_map); // BUG ICI
//         //auto fm = prediction(f, max_level, 0, i, 1, tag, memoization_map);

//         // CELA IL FAUT LE TRAITER MAIS ON VERRA APRES...

//         // auto rho = xt::eval(fp + fm);

//         // error_cell_by_cell(0, max_level, i) = dx * xt::abs(rho);
//     });

//     return error_to_return;
    
    
//     //return xt::sum(xt::view(error_cell_by_cell, 0, max_level, xt::all));
// }

template<class Field, class FieldR>
std::array<double, 2> compute_error(Field & f, FieldR & fR, double t)
{

    auto mesh = f.mesh();
    auto max_level = mesh.max_level();

    auto meshR = fR.mesh();

    mure::mr_projection(f);
    mure::mr_prediction(f);  // C'est supercrucial de le faire.

    // Getting ready for memoization
    using interval_t = typename Field::Config::interval_t;
    std::map<std::tuple<std::size_t, std::size_t, interval_t>, xt::xtensor<double, 2>> error_memoization_map;
    error_memoization_map.clear();

    double error = 0; // To return
    double diff = 0.0;

    double dx = 1.0 / (1 << max_level);

    for (std::size_t level = 0; level <= max_level; ++level)
    {
        auto exp = mure::intersection(mesh[mure::MeshType::cells][level],
                                      mesh[mure::MeshType::cells][level]);

        exp([&](auto, auto &interval, auto) {
            auto i = interval[0];

            auto j = max_level - level;

            auto int_tmp = i * (1 << j);
            int_tmp.step = 1;

            auto fp = prediction_all(f, level, j, int_tmp, error_memoization_map);

            xt::xtensor<double, 1> x = dx*xt::linspace<int>(int_tmp.start, int_tmp.end - 1, int_tmp.size()) + 0.5*dx;
            xt::xtensor<double, 1> uexact = (x >= -1.0 and x < t) * ((1 + x) / (1 + t)) + 
                                            (x >= t and x < 1) * (1 - x) / (1 - t);
            error += xt::sum(xt::abs(xt::sum(fp, 1) - uexact))[0];
        });
    }

    return {dx * error, 0.0}; // Normalization by dx before returning
    // I think it is better to do the normalization at the very end ... especially for round-offs    
}

int main(int argc, char *argv[])
{
    cxxopts::Options options("lbm_d1q2_burgers",
                             "Multi resolution for a D1Q2 LBM scheme for Burgers equation");

    options.add_options()
                       ("min_level", "minimum level", cxxopts::value<std::size_t>()->default_value("2"))
                       ("max_level", "maximum level", cxxopts::value<std::size_t>()->default_value("10"))
                       ("epsilon", "maximum level", cxxopts::value<double>()->default_value("0.01"))
                       ("log", "log level", cxxopts::value<std::string>()->default_value("warning"))
                       ("h, help", "Help");

    try
    {
        auto result = options.parse(argc, argv);

        if (result.count("help"))
            std::cout << options.help() << "\n";
        else
        {
            std::map<std::string, spdlog::level::level_enum> log_level{{"debug", spdlog::level::debug},
                                                               {"warning", spdlog::level::warn}};
            constexpr size_t dim = 1;
            using Config = mure::MRConfig<dim, 2>;

            spdlog::set_level(log_level[result["log"].as<std::string>()]);
            std::size_t min_level = result["min_level"].as<std::size_t>();
            std::size_t max_level = result["max_level"].as<std::size_t>();
            double eps = result["epsilon"].as<double>();

            mure::Box<double, dim> box({-3}, {3});
            mure::Mesh<Config> mesh{box, min_level, max_level};
            mure::Mesh<Config> meshR{box, max_level, max_level}; // This is the reference scheme

            // Initialization
            auto f  = init_f(mesh , 0.0);
            auto fR = init_f(meshR, 0.0);

            double T = 1.2;
            double dx = 1.0 / (1 << max_level);
            double dt = dx;

            std::size_t N = static_cast<std::size_t>(T / dt);

            double t = 0.0;


            for (std::size_t nb_ite = 0; nb_ite < 1; ++nb_ite)
            {

                // For the reference solution, we just call a coarsening
                // in order to set the BC and the ghost correctly
                coarsening(fR, 0.0, 0); // and thats all

                tic();
                for (std::size_t i=0; i<max_level-min_level; ++i)
                {
                    if (coarsening(f, eps, i))
                        break;
                }
                auto duration_coarsening = toc();

                // save_solution(f, eps, nb_ite, "coarsening");

                tic();
                for (std::size_t i=0; i<max_level-min_level; ++i)
                {
                    if (refinement(f, eps, i))
                        break;
                }
                auto duration_refinement = toc();
                //save_solution(f, eps, nb_ite, "refinement");


                // Create and initialize field containing the leaves
                tic();
                mure::Field<Config, int, 1> tag_leaf{"tag_leaf", mesh};
                tag_leaf.array().fill(0);
                mesh.for_each_cell([&](auto &cell) {
                    tag_leaf[cell] = static_cast<int>(1);
                });
                auto duration_leaf_checking = toc();

                mure::Field<Config, int, 1> tag_leafR{"tag_leafR", meshR};
                tag_leafR.array().fill(0);
                meshR.for_each_cell([&](auto &cell) {
                    tag_leafR[cell] = static_cast<int>(1);
                });

                auto error = compute_error(f, fR, t);

                std::cout<<std::endl;

                
                tic();
                one_time_step(f, tag_leaf);
                auto duration_scheme = toc();

                tic();
                one_time_step(fR, tag_leafR);
                auto duration_schemeR = toc();

                t += dt;

                tic();
                save_solution(f, eps, nb_ite, "onetimestep");
                auto duration_save = toc();


                std::cout<<std::endl<<"\n=======Iteration "<<nb_ite<<" summary========"
                                    <<"\nCoarsening: "<<duration_coarsening
                                    <<"\nRefinement: "<<duration_refinement
                                    <<"\nLeafChecking: "<<duration_leaf_checking
                                    <<"\nScheme: "<<duration_scheme
                                    <<"\nScheme reference: "<<duration_schemeR
                                    <<"\nSave: "<<duration_save
                                    <<"\nError exact - adaptive = "<< error[0] << "\n";
                                    //<<"\nNorm after computation = "<<norm_after_computation;
                                    

            }
        }
    }
    catch (const cxxopts::OptionException &e)
    {
        std::cout << options.help() << "\n";
    }

    return 0;
}
