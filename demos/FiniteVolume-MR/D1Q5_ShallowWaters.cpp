#include <math.h>
#include <vector>
#include <fstream>

#include <cxxopts.hpp>
#include <spdlog/spdlog.h>

#include <xtensor/xio.hpp>

#include <mure/mure.hpp>
#include "coarsening.hpp"
#include "refinement.hpp"
#include "criteria.hpp"

#include "harten.hpp"
#include "prediction_map_1d.hpp"


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

template<class coord_index_t>
auto compute_prediction_separate_inout(std::size_t min_level, std::size_t max_level)
{
    coord_index_t i = 0;
    std::vector<std::vector<prediction_map<coord_index_t>>> data(max_level-min_level+1);

    for(std::size_t k=0; k<max_level-min_level+1; ++k)
    {
        int size = (1<<k);
        data[k].resize(8);

        data[k][0] = prediction(k, i*size - 1);
        data[k][1] = prediction(k, (i+1)*size - 1);
        data[k][2] = prediction(k, (i+1)*size);
        data[k][3] = prediction(k, i*size);


        if (k == 0) {
            data[k][4] = prediction(k, i - 2);
            data[k][5] = prediction(k, i);
            data[k][6] = prediction(k, i + 2);
            data[k][7] = prediction(k, i);
        }
        else
        {
            data[k][4] = prediction(k, i*size - 2) + prediction(k, i*size - 1);
            data[k][5] = prediction(k, (i+1)*size - 1) + prediction(k, (i+1)*size - 2);
            data[k][6] = prediction(k, (i+1)*size) + prediction(k, (i+1)*size + 1);
            data[k][7] = prediction(k, i*size) + prediction(k, i*size + 1);
        }
        




    }
    return data;
}

std::array<double, 2> exact_solution(double x, double t)   {

    double g = 1.0;
    double x0 = 0.0;

    double hL = 2.0;
    double hR = 1.0;
    double uL = 0.0;
    double uR = 0.0;

    double cL = std::sqrt(g*hL);
    double cR = std::sqrt(g*hR);
    double cStar = 1.20575324689; // To be computed
    double hStar = cStar*cStar / g;

    double xFanL = x0 - cL * t;
    double xFanR = x0 + (2*cL - 3*cStar) * t;
    double xShock = x0 + (2*cStar*cStar*(cL - cStar)) / (cStar*cStar - cR*cR) * t;

    double h = (x <= xFanL) ? hL : ((x <= xFanR) ? 4./(9.*g)*pow(cL-(x-x0)/(2.*t), 2.0) : ((x < xShock) ? hStar : hR));
    double u = (x <= xFanL) ? uL : ((x <= xFanR) ? 2./3.*(cL+(x-x0)/t) : ((x < xShock) ? 2.*(cL - cStar) : uR));

    return {h, u};
    
}

template<class Config>
auto init_f(mure::Mesh<Config> &mesh, double t)
{
    constexpr std::size_t nvel = 5;
    mure::BC<1> bc{ {{ {mure::BCType::neumann, 0.0},
                       {mure::BCType::neumann, 0.0},
                       {mure::BCType::neumann, 0.0},
                       {mure::BCType::neumann, 0.0},
                       {mure::BCType::neumann, 0.0},
                    }} };

    mure::Field<Config, double, nvel> f("f", mesh, bc);
    f.array().fill(0);

    mesh.for_each_cell([&](auto &cell) {
        auto center = cell.center();
        auto x = center[0];

        double g = 1.0;

        auto u = exact_solution(x, 0.0);

        double lambda = 2.0;


        double h = u[0];
        double q = h * u[1]; // Linear momentum
        double k = q*q/h + 0.5*g*h*h; // Energy
        double v = 1.0 * q * lambda*lambda; // Fourth mom
        double z = 1.0 * (q*q/h + 0.5*g*h*h) * lambda*lambda; // Fifth mom

        // Just to have a shorthand
        double lb1 = lambda;
        double lb2 = lambda * lb1;
        double lb3 = lambda * lb2;
        double lb4 = lambda * lb3;

        f[cell][0] = 1.0*h +                - 5./(4.*lb2) *k                  + 1./(4.*lb4) *z;        
        f[cell][1] =         2./(3.*lb1) *q + 2./(3.*lb2) *k - 1./(6.*lb3) *v - 1./(6.*lb4) *z;        
        f[cell][2] =       - 2./(3.*lb1) *q + 2./(3.*lb2) *k + 1./(6.*lb3) *v - 1./(6.*lb4) *z; 
        f[cell][3] =       - 1./(12.*lb1)*q - 1./(24.*lb2)*k + 1./(12.*lb3)*v + 1./(24.*lb4)*z;      
        f[cell][4] =         1./(12.*lb1)*q - 1./(24.*lb2)*k - 1./(12.*lb3)*v + 1./(24.*lb4)*z;      

    });

    return f;
}

template<class Field, class interval_t>
xt::xtensor<double, 1> prediction(const Field& f, std::size_t level_g, std::size_t level, const interval_t &i, const std::size_t item, 
                                  std::map<std::tuple<std::size_t, std::size_t, std::size_t, interval_t>, 
                                  xt::xtensor<double, 1>> & mem_map)
{

    // We check if the element is already in the map
    auto it = mem_map.find({item, level_g, level, i});
    if (it != mem_map.end())   {
        //std::cout<<std::endl<<"Found by memoization";
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

    
        auto val = xt::eval(prediction(f, level_g, level-1, ig, item, mem_map) - 1./8 * d * (prediction(f, level_g, level-1, ig+1, item, mem_map) 
                                                                                       - prediction(f, level_g, level-1, ig-1, item, mem_map)));
        

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

template<class Field>
void one_time_step(Field &f, double s)
{
    constexpr std::size_t nvel = Field::size;
    double lambda = 2.;//, s = 1.0;
    auto mesh = f.mesh();
    auto max_level = mesh.max_level();

    mure::mr_projection(f);
    f.update_bc();
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


            // STREAM

            std::size_t j = max_level - level;

            double coeff = 1. / (1 << j);

            // This is the STANDARD FLUX EVALUATION

            auto f0 = f(0, level, i);
            
            auto fp = f(1, level, i) + coeff * (prediction(f, level, j, i*(1<<j)-1, 1, memoization_map)
                                             -  prediction(f, level, j, (i+1)*(1<<j)-1, 1, memoization_map));

            auto fm = f(2, level, i) - coeff * (prediction(f, level, j, i*(1<<j), 2, memoization_map)
                                             -  prediction(f, level, j, (i+1)*(1<<j), 2, memoization_map));

            auto fpp = f(3, level, i) + coeff * (prediction(f, level, j, i*(1<<j)-2, 3, memoization_map)
                                               + prediction(f, level, j, i*(1<<j)-1, 3, memoization_map)
                                               - prediction(f, level, j, (i+1)*(1<<j)-2, 3, memoization_map)
                                               - prediction(f, level, j, (i+1)*(1<<j)-1, 3, memoization_map));

            auto fmm = f(4, level, i) - coeff * (prediction(f, level, j, i*(1<<j), 4, memoization_map)
                                               + prediction(f, level, j, i*(1<<j) + 1, 4, memoization_map)
                                               - prediction(f, level, j, (i+1)*(1<<j), 4, memoization_map)
                                               - prediction(f, level, j, (i+1)*(1<<j) + 1, 4, memoization_map));


            // COLLISION    

            double lb1 = lambda;
            double lb2 = lambda * lb1;
            double lb3 = lambda * lb2;
            double lb4 = lambda * lb3;

            auto h = xt::eval(f0 + fp + fm + fpp + fmm);
            auto q = xt::eval(lb1 * (fp - fm + 2*fpp - 2*fmm));
            auto k = xt::eval(lb2 * (fp + fm + 4*fpp + 4*fmm));
            auto v = xt::eval(lb3 * (fp - fm + 8*fpp - 8*fmm));
            auto z = xt::eval(lb4 * (fp + fm + 16*fpp + 16*fmm));

            double g = 1.0;
            double s3 = 1.0;
            double s4 = 1.0;


            auto k_coll = (1 - s) * k + s * (q*q/h + 0.5*g*h*h);
            auto v_coll = (1 - s3) * v + s3 * (1.0 * q * lambda*lambda);
            auto z_coll = (1 - s4) * z + s4 * (1.0 * (q*q/h + 0.5*g*h*h) * lambda*lambda);


            new_f(0, level, i) = 1.0*h +                - 5./(4.*lb2) *k_coll                       + 1./(4.*lb4) *z_coll; 
            new_f(1, level, i) =         2./(3.*lb1) *q + 2./(3.*lb2) *k_coll - 1./(6.*lb3) *v_coll - 1./(6.*lb4) *z_coll;
            new_f(2, level, i) =       - 2./(3.*lb1) *q + 2./(3.*lb2) *k_coll + 1./(6.*lb3) *v_coll - 1./(6.*lb4) *z_coll;
            new_f(3, level, i) =       - 1./(12.*lb1)*q - 1./(24.*lb2)*k_coll + 1./(12.*lb3)*v_coll + 1./(24.*lb4)*z_coll;
            new_f(4, level, i) =         1./(12.*lb1)*q - 1./(24.*lb2)*k_coll - 1./(12.*lb3)*v_coll + 1./(24.*lb4)*z_coll;

        });
    }

    std::swap(f.array(), new_f.array());
}



template<class Field, class Pred>
void one_time_step_matrix_overleaves(Field &f, const Pred& pred_coeff, double s_rel)
{

    double value_dirichlet = 0.;

    double lambda = 2.;

    constexpr std::size_t nvel = Field::size;
    using coord_index_t = typename Field::coord_index_t;

    auto mesh = f.mesh();
    auto max_level = mesh.max_level();

    mure::mr_projection(f);
    f.update_bc();
    mure::mr_prediction(f);

    // After that everything is ready, we predict what is remaining
    mure::mr_prediction_overleaves(f);

    Field new_f{"new_f", mesh};
    new_f.array().fill(0.);

    Field help_f{"help_f", mesh};
    help_f.array().fill(0.);

    for (std::size_t level = 0; level <= max_level; ++level)
    {


        // If we are at the finest level, we no not need to correct
        if (level == max_level) {
            std::size_t j = 0; 
            double coeff = 1.;



            auto leaves = mure::intersection(mesh[mure::MeshType::cells][max_level],
                                             mesh[mure::MeshType::cells][max_level]);
            leaves.on(max_level)([&](auto, auto &interval, auto) {

                auto i = interval[0]; 

                auto f0  = xt::eval(f(0, max_level, i));
                auto fp  = xt::eval(f(1, max_level, i - 1));
                auto fm  = xt::eval(f(2, max_level, i + 1));
                auto fpp = xt::eval(f(3, max_level, i - 2));
                auto fmm = xt::eval(f(4, max_level, i + 2));

              // COLLISION    

                double lb1 = lambda;
                double lb2 = lambda * lb1;
                double lb3 = lambda * lb2;
                double lb4 = lambda * lb3;

                auto h = xt::eval(f0 + fp + fm + fpp + fmm);
                auto q = xt::eval(lb1 * (fp - fm + 2*fpp - 2*fmm));
                auto k = xt::eval(lb2 * (fp + fm + 4*fpp + 4*fmm));
                auto v = xt::eval(lb3 * (fp - fm + 8*fpp - 8*fmm));
                auto z = xt::eval(lb4 * (fp + fm + 16*fpp + 16*fmm));

                double g = 1.0;
                double s3 = 1.0;
                double s4 = 1.0;


                auto k_coll = (1 - s_rel) * k + s_rel * (q*q/h + 0.5*g*h*h);
                auto v_coll = (1 - s3) * v + s3 * (1.0 * q * lambda*lambda);
                auto z_coll = (1 - s4) * z + s4 * (1.0 * (q*q/h + 0.5*g*h*h) * lambda*lambda);


                new_f(0, level, i) = 1.0*h +                - 5./(4.*lb2) *k_coll                       + 1./(4.*lb4) *z_coll; 
                new_f(1, level, i) =         2./(3.*lb1) *q + 2./(3.*lb2) *k_coll - 1./(6.*lb3) *v_coll - 1./(6.*lb4) *z_coll;
                new_f(2, level, i) =       - 2./(3.*lb1) *q + 2./(3.*lb2) *k_coll + 1./(6.*lb3) *v_coll - 1./(6.*lb4) *z_coll;
                new_f(3, level, i) =       - 1./(12.*lb1)*q - 1./(24.*lb2)*k_coll + 1./(12.*lb3)*v_coll + 1./(24.*lb4)*z_coll;
                new_f(4, level, i) =         1./(12.*lb1)*q - 1./(24.*lb2)*k_coll - 1./(12.*lb3)*v_coll + 1./(24.*lb4)*z_coll;

            });
        }

        // Otherwise, correction is needed
        else
        {

            // We do the advection on the overleaves
            std::size_t j = max_level - (level + 1); 
            double coeff = 1. / (1 << j);

            // We take the overleaves corresponding to the existing leaves
            // auto overleaves = mure::intersection(mesh[mure::MeshType::cells][level],
            //                                      mesh[mure::MeshType::cells][level]).on(level + 1);

            auto ol = mure::intersection(mesh[mure::MeshType::cells][level],
                                                 mesh[mure::MeshType::cells][level]).on(level + 1);

            
            ol([&](auto, auto &interval, auto) {
                auto k = interval[0]; // Logical index in x


                auto f0  = xt::eval(f(0, level + 1, k));
                auto fp  = xt::eval(f(1, level + 1, k));
                auto fm  = xt::eval(f(2, level + 1, k));
                auto fpp = xt::eval(f(3, level + 1, k));
                auto fmm = xt::eval(f(4, level + 1, k));

                for(auto &c: pred_coeff[j][0].coeff)
                {
                    coord_index_t stencil = c.first;
                    double weight = c.second;

                    fp += coeff * weight * f(1, level + 1, k + stencil);
                }

                for(auto &c: pred_coeff[j][1].coeff)
                {
                    coord_index_t stencil = c.first;
                    double weight = c.second;

                    fp -= coeff * weight * f(1, level + 1, k + stencil);
                }

                for(auto &c: pred_coeff[j][2].coeff)
                {
                    coord_index_t stencil = c.first;
                    double weight = c.second;

                    fm += coeff * weight * f(2, level + 1, k + stencil);
                }

                for(auto &c: pred_coeff[j][3].coeff)
                {
                    coord_index_t stencil = c.first;
                    double weight = c.second;

                    fm -= coeff * weight * f(2, level + 1, k + stencil);
                }


                for(auto &c: pred_coeff[j][4].coeff)
                {
                    coord_index_t stencil = c.first;
                    double weight = c.second;

                    fpp += coeff * weight * f(3, level + 1, k + stencil);
                }

                for(auto &c: pred_coeff[j][5].coeff)
                {
                    coord_index_t stencil = c.first;
                    double weight = c.second;

                    fpp -= coeff * weight * f(3, level + 1, k + stencil);
                }

                for(auto &c: pred_coeff[j][6].coeff)
                {
                    coord_index_t stencil = c.first;
                    double weight = c.second;

                    fmm += coeff * weight * f(4, level + 1, k + stencil);
                }

                for(auto &c: pred_coeff[j][7].coeff)
                {
                    coord_index_t stencil = c.first;
                    double weight = c.second;

                    fmm -= coeff * weight * f(4, level + 1, k + stencil);
                }





                // Save it
                help_f(0, level + 1, k) = f0;
                help_f(1, level + 1, k) = fp;
                help_f(2, level + 1, k) = fm;
                help_f(3, level + 1, k) = fpp;
                help_f(4, level + 1, k) = fmm;

            });

            // Now that projection has been done, we have to come back on the leaves below the overleaves
            auto leaves = mure::intersection(mesh[mure::MeshType::cells][level],
                                             mesh[mure::MeshType::cells][level]);

            leaves([&](auto, auto &interval, auto) {
                auto i = interval[0]; 

                // Projection
                auto f0_advected  = 0.5 * (help_f(0, level + 1, 2*i) + help_f(0, level + 1, 2*i + 1));
                auto fp_advected  = 0.5 * (help_f(1, level + 1, 2*i) + help_f(1, level + 1, 2*i + 1));
                auto fm_advected  = 0.5 * (help_f(2, level + 1, 2*i) + help_f(2, level + 1, 2*i + 1));
                auto fpp_advected = 0.5 * (help_f(3, level + 1, 2*i) + help_f(3, level + 1, 2*i + 1));
                auto fmm_advected = 0.5 * (help_f(4, level + 1, 2*i) + help_f(4, level + 1, 2*i + 1));

                 // COLLISION    

                double lb1 = lambda;
                double lb2 = lambda * lb1;
                double lb3 = lambda * lb2;
                double lb4 = lambda * lb3;

                auto h = xt::eval(f0_advected + fp_advected + fm_advected + fpp_advected + fmm_advected);
                auto q = xt::eval(lb1 * (fp_advected - fm_advected + 2*fpp_advected - 2*fmm_advected));
                auto k = xt::eval(lb2 * (fp_advected + fm_advected + 4*fpp_advected + 4*fmm_advected));
                auto v = xt::eval(lb3 * (fp_advected - fm_advected + 8*fpp_advected - 8*fmm_advected));
                auto z = xt::eval(lb4 * (fp_advected + fm_advected + 16*fpp_advected + 16*fmm_advected));

                double g = 1.0;
                double s3 = 1.0;
                double s4 = 1.0;


                auto k_coll = (1 - s_rel) * k + s_rel * (q*q/h + 0.5*g*h*h);
                auto v_coll = (1 - s3) * v + s3 * (1.0 * q * lambda*lambda);
                auto z_coll = (1 - s4) * z + s4 * (1.0 * (q*q/h + 0.5*g*h*h) * lambda*lambda);


                new_f(0, level, i) = 1.0*h +                - 5./(4.*lb2) *k_coll                       + 1./(4.*lb4) *z_coll; 
                new_f(1, level, i) =         2./(3.*lb1) *q + 2./(3.*lb2) *k_coll - 1./(6.*lb3) *v_coll - 1./(6.*lb4) *z_coll;
                new_f(2, level, i) =       - 2./(3.*lb1) *q + 2./(3.*lb2) *k_coll + 1./(6.*lb3) *v_coll - 1./(6.*lb4) *z_coll;
                new_f(3, level, i) =       - 1./(12.*lb1)*q - 1./(24.*lb2)*k_coll + 1./(12.*lb3)*v_coll + 1./(24.*lb4)*z_coll;
                new_f(4, level, i) =         1./(12.*lb1)*q - 1./(24.*lb2)*k_coll - 1./(12.*lb3)*v_coll + 1./(24.*lb4)*z_coll;

            });   
        }
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
    str << "LBM_D1Q5_ShallowWaters_" << ext << "_lmin_" << min_level << "_lmax-" << max_level << "_eps-"
        << eps << "_ite-" << ite;

    auto h5file = mure::Hdf5(str.str().data());
    h5file.add_mesh(mesh);
    mure::Field<Config> level_{"level", mesh};
    mure::Field<Config> h{"h", mesh};
    mure::Field<Config> q{"q", mesh};

    double lambda = 2.;

    mesh.for_each_cell([&](auto &cell) {
        level_[cell] = static_cast<double>(cell.level);
        h[cell] = f[cell][0] + f[cell][1] + f[cell][2] + f[cell][3] + f[cell][4];
        q[cell] = lambda*(f[cell][1] - f[cell][2] + 2*f[cell][3] - 2*f[cell][4]);

    });
    h5file.add_field(h);
    h5file.add_field(q);
    h5file.add_field(f);
    h5file.add_field(level_);
}



// Attention : the number 2 as second template parameter does not mean
// that we are dealing with two fields!!!!
template<class Field, class interval_t>
xt::xtensor<double, 2> prediction_all(const Field & f, std::size_t level_g, std::size_t level, 
                                      const interval_t & k, 
                                      std::map<std::tuple<std::size_t, std::size_t, interval_t>, xt::xtensor<double, 2>> & mem_map)
{

    // That is used to employ _ with xtensor
    using namespace xt::placeholders;

    auto it = mem_map.find({level_g, level, k});


    if (it != mem_map.end() && k.size() == (std::get<2>(it->first)).size())    {

        return it->second;
    }
    else
    {
        

    auto mesh = f.mesh();

    // We put only the size in x (k.size()) because in y
    // we only have slices of size 1. 
    // The second term (1) should be adapted according to the 
    // number of fields that we have.
    // std::vector<std::size_t> shape_x = {k.size(), 4};
    std::vector<std::size_t> shape_x = {k.size(), 5};
    xt::xtensor<double, 2> out = xt::empty<double>(shape_x);

    auto mask = mesh.exists(mure::MeshType::cells_and_ghosts, level_g + level, k); // Check if we are on a leaf or a ghost (CHECK IF IT IS OK)

    xt::xtensor<double, 2> mask_all = xt::empty<double>(shape_x);
        
    // for (int h_field = 0; h_field < 4; ++h_field)  {
    for (int h_field = 0; h_field < 5; ++h_field)  {
        xt::view(mask_all, xt::all(), h_field) = mask;
    }    

    // Recursion finished
    if (xt::all(mask))
    {                 
        return xt::eval(f(0, 5, level_g + level, k));

    }

    // If we cannot stop here

    auto kg = k >> 1;
    kg.step = 1;

    xt::xtensor<double, 2> val = xt::empty<double>(shape_x);



    auto earth  = xt::eval(prediction_all(f, level_g, level - 1, kg     , mem_map));
    auto W      = xt::eval(prediction_all(f, level_g, level - 1, kg - 1 , mem_map));
    auto E      = xt::eval(prediction_all(f, level_g, level - 1, kg + 1 , mem_map));
   


    // This is to deal with odd/even indices in the x direction
    std::size_t start_even = (k.start & 1) ?     1         :     0        ; 
    std::size_t start_odd  = (k.start & 1) ?     0         :     1        ; 
    std::size_t end_even   = (k.end & 1)   ? kg.size()     : kg.size() - 1;
    std::size_t end_odd    = (k.end & 1)   ? kg.size() - 1 : kg.size()    ;


    
    xt::view(val, xt::range(start_even, _, 2)) = xt::view(                        earth 
                                                          + 1./8               * (W - E), xt::range(start_even, _));



    xt::view(val, xt::range(start_odd, _, 2))  = xt::view(                        earth 
                                                          - 1./8               * (W - E), xt::range(_, end_odd));

    xt::masked_view(out, !mask_all) = xt::masked_view(val, !mask_all);

    for(int k_mask = 0, k_int = k.start; k_int < k.end; ++k_mask, ++k_int)
    {
        if (mask[k_mask])
        {
            xt::view(out, k_mask) = xt::view(f(0, 5, level_g + level, {k_int, k_int + 1}), 0);

        }
    }

    // It is crucial to use insert and not []
    // in order not to update the value in case of duplicated (same key)
    mem_map.insert(std::make_pair(std::tuple<std::size_t, std::size_t, interval_t>{level_g, level, k}
                                  ,out));


    return out;

    }
}

template<class Config, class FieldR>
std::array<double, 4> compute_error(mure::Field<Config, double, 5> &f, FieldR & fR, double t)
{

    auto mesh = f.mesh();

    auto meshR = fR.mesh();
    auto max_level = meshR.max_level();

    mure::mr_projection(f);
    f.update_bc(); // Important especially when we enforce Neumann...for the Riemann problem
    mure::mr_prediction(f);  // C'est supercrucial de le faire.


    // Getting ready for memoization
    // using interval_t = typename Field::Config::interval_t;
    using interval_t = typename Config::interval_t;
    std::map<std::tuple<std::size_t, std::size_t, interval_t>, xt::xtensor<double, 2>> error_memoization_map;
    error_memoization_map.clear();

    double error_h = 0.0; // First momentum 
    double error_q = 0.0; // Second momentum
    double diff_h = 0.0;
    double diff_q = 0.0;


    double dx = 1.0 / (1 << max_level);

    for (std::size_t level = 0; level <= max_level; ++level)
    {
        auto exp = mure::intersection(meshR[mure::MeshType::cells][max_level],
                                      mesh[mure::MeshType::cells][level])
                  .on(max_level);

        exp([&](auto, auto &interval, auto) {
            auto i = interval[0];
            auto j = max_level - level;

            auto sol  = prediction_all(f, level, j, i, error_memoization_map);
            auto solR = xt::view(fR(max_level, i), xt::all(), xt::range(0, 3));


            xt::xtensor<double, 1> x = dx*xt::linspace<int>(i.start, i.end - 1, i.size()) + 0.5*dx;


            xt::xtensor<double, 1> hexact = xt::zeros<double>(x.shape());
            xt::xtensor<double, 1> qexact = xt::zeros<double>(x.shape());

            for (std::size_t idx = 0; idx < x.shape()[0]; ++idx)    {
                auto ex_sol = exact_solution(x[idx], t);

                hexact[idx] = ex_sol[0];
                qexact[idx] = ex_sol[0]*ex_sol[1];

            }

            double lambda = 2.0;

            
            
            auto h =  xt::eval(xt::view(sol, xt::all(), 0) +  xt::view(sol, xt::all(), 1) + xt::view(sol, xt::all(), 2)
                                                           +  xt::view(sol, xt::all(), 3) + xt::view(sol, xt::all(), 4));

            auto q =  lambda * xt::eval(xt::view(sol, xt::all(), 1) - xt::view(sol, xt::all(), 2)
                             + 2. * xt::view(sol, xt::all(), 1) - 2.*xt::view(sol, xt::all(), 2));


            auto h_ref =  xt::eval(fR(0, max_level, i) + fR(1, max_level, i) + fR(2, max_level, i)
                                                       + fR(3, max_level, i) + fR(4, max_level, i));
            auto q_ref =  lambda * xt::eval(fR(1, max_level, i) - fR(2, max_level, i)
                                          + 2.*fR(1, max_level, i) - 2.*fR(2, max_level, i));


            error_h += xt::sum(xt::abs(h_ref - hexact))[0];

            error_q += xt::sum(xt::abs(q_ref - qexact))[0];


            diff_h += xt::sum(xt::abs(h_ref - h))[0];
            
            diff_q += xt::sum(xt::abs(q_ref - q))[0];
            
        });
    }


    return {dx * error_h, dx * diff_h, 
            dx * error_q, dx * diff_q};


}

int main(int argc, char *argv[])
{
    cxxopts::Options options("lbm_d1q3_shallow waters",
                             "...");

    options.add_options()
                       ("min_level", "minimum level", cxxopts::value<std::size_t>()->default_value("2"))
                       ("max_level", "maximum level", cxxopts::value<std::size_t>()->default_value("10"))
                       ("epsilon", "maximum level", cxxopts::value<double>()->default_value("0.01"))
                       ("s", "relaxation parameter", cxxopts::value<double>()->default_value("1.0"))
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
            //using Config = mure::MRConfig<dim, 2>;
            using Config = mure::MRConfig<dim, 2>;

            spdlog::set_level(log_level[result["log"].as<std::string>()]);
            std::size_t min_level = result["min_level"].as<std::size_t>();
            std::size_t max_level = result["max_level"].as<std::size_t>();
            double eps = result["epsilon"].as<double>();
            double s = result["s"].as<double>();


            mure::Box<double, dim> box({-1}, {1});
            mure::Mesh<Config> mesh{box, min_level, max_level};
            mure::Mesh<Config> meshR{box, max_level, max_level}; // This is the reference scheme

            using coord_index_t = typename Config::coord_index_t;
            auto pred_coeff_separate = compute_prediction_separate_inout<coord_index_t>(min_level, max_level);



            // Initialization
            auto f   = init_f(mesh , 0.0);
            auto fR  = init_f(meshR , 0.0);

            double T = 0.6;

            double lambda = 2.0;

            double dx = 1.0 / (1 << max_level);
            double dt = dx / lambda;

            std::size_t N = static_cast<std::size_t>(T / dt);

            double t = 0.0;

            // xt::xtensor<double, 2> test = xt::empty<double>({10, 3});

            // std::cout<<std::endl<<test;
            // return 0;

            for (std::size_t nb_ite = 0; nb_ite < N; ++nb_ite)
            {

                std::cout<<std::endl<<"Iteration "<<nb_ite<<" Time = "<<t;
                
                // tic();
                // for (std::size_t i=0; i<max_level-min_level; ++i)
                // {
                //     //std::cout<<std::endl<<"Passe "<<i;
                //     if (coarsening(f, eps, i))
                //         break;
                // }
                // auto duration_coarsening = toc();

                // // save_solution(f, eps, nb_ite, "coarsening");

                // tic();
                // for (std::size_t i=0; i<max_level-min_level; ++i)
                // {
                //     if (refinement(f, eps, 0.0, i))
                //         break;
                // }
                // auto duration_refinement = toc();


                auto mesh_old = mesh;
                mure::Field<Config, double, 5> f_old{"u", mesh_old};
                f_old.array() = f.array();
                for (std::size_t i=0; i<max_level-min_level; ++i)
                {
                    std::cout<<std::endl<<"Step "<<i<<std::flush;
                    if (harten(f, f_old, eps, 0., i, nb_ite))
                        break;
                }

                save_solution(f, eps, nb_ite, "refinement");



                auto error = compute_error(f, fR, t);


                std::cout<<std::endl<<"Error h = "<<error[0]<<std::endl
                                    <<"Diff h = "<<error[1]<<std::endl
                                    <<"Error q = "<<error[2]<<std::endl
                                    <<"Diff q = "<<error[3];

                

                
                tic();
                // one_time_step(f, s);
                one_time_step_matrix_overleaves(f, pred_coeff_separate, s);

                auto duration_scheme = toc();

                // one_time_step(fR, s);
                one_time_step_matrix_overleaves(fR, pred_coeff_separate, s);



                t += dt;

                tic();
                save_solution(f, eps, nb_ite, "onetimestep");
                auto duration_save = toc();


                                    

            }
            


        }

    }
    catch (const cxxopts::OptionException &e)
    {
        std::cout << options.help() << "\n";
    }

    std::cout<<std::endl;


    return 0;
}
