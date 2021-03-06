/*
 * IRIS Localization and Mapping (LaMa)
 *
 * Copyright (c) 2019-today, Eurico Pedrosa, University of Aveiro - Portugal
 * All rights reserved.
 * License: New BSD
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the University of Aveiro nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <iostream>
#include <fstream>

#include "lama/print.h"
#include "lama/random.h"

#include "lama/match_surface_2d.h"
#include "lama/loc2d.h"

#include "lama/nlls/gauss_newton.h"
#include "lama/nlls/levenberg_marquardt.h"

lama::Loc2D::Options::Options()
{
    trans_thresh = 0.5;
    rot_thresh   = 0.5;
    l2_max       = 1.0;
    resolution   = 0.05;
    patch_size   = 32;
    max_iter     = 100;
}


void lama::Loc2D::Init(const Options& options)
{
    occupancy_map = new SimpleOccupancyMap(options.resolution, options.patch_size, false);
    distance_map  = new DynamicDistanceMap(options.resolution, options.patch_size, false);
    distance_map->setMaxDistance(options.l2_max);

    /* solver_options_.write_to_stdout= true; */
    solver_options_.max_iterations = options.max_iter;
    solver_options_.strategy       = makeStrategy(options.strategy, Vector2d::Zero());
    /* solver_options_.robust_cost    = makeRobust("cauchy", 0.25); */
    solver_options_.robust_cost.reset(new CauchyWeight(0.15));

    trans_thresh_ = options.trans_thresh;
    rot_thresh_   = options.rot_thresh;

    has_first_scan = false;
    do_global_loclization_ = false;
}

lama::Loc2D::~Loc2D()
{}

bool lama::Loc2D::enoughMotion(const Pose2D& odometry)
{
    if (not has_first_scan)
        return true;

    Pose2D odelta = odom_ - odometry;

    if (odelta.xy().norm() <= trans_thresh_ && std::abs(odelta.rotation()) <= rot_thresh_)
        return false;

    return true;
}

bool lama::Loc2D::update(const PointCloudXYZ::Ptr& surface, const Pose2D& odometry, double timestamp)
{
    if (not has_first_scan){
        odom_ = odometry;

        has_first_scan = true;
        return true;
    }

    // 1. Predict from odometry
    Pose2D odelta = odom_ - odometry;
    Pose2D ppose  = pose_ + odelta;

    // only continue if the necessary motion was gathered.
    if (odelta.xy().norm() <= trans_thresh_ &&
        std::abs(odelta.rotation()) <= rot_thresh_)
        return false;

    pose_ = ppose;
    odom_ = odometry;

    if (do_global_loclization_)
        globalLocalization(surface);

    // 2. Optimize
    MatchSurface2D match_surface(distance_map, surface, pose_.state);
    Solve(solver_options_, match_surface, 0);

    //--
    if (do_global_loclization_){
        VectorXd residuals;
        match_surface.eval(residuals, 0);
        double rmse = sqrt(residuals.squaredNorm()/((double)(surface->points.size() - 1)));

        if (rmse < 0.15)
            do_global_loclization_ = false;
    }
    //--

    pose_.state = match_surface.getState();

    return true;
}

void lama::Loc2D::triggerGlobalLocalization()
{
    do_global_loclization_ = true;
}


void lama::Loc2D::globalLocalization(const PointCloudXYZ::Ptr& surface)
{
    Vector3d min, max;
    occupancy_map->bounds(min, max);

    Vector3d diff = max - min;

    double best_error = std::numeric_limits<double>::max();

    const size_t numOfParticles = 3000;
    for (size_t i = 0; i < numOfParticles; ++i){

        double x, y, a;

        for (;;){
            x = min[0] + random::uniform() * diff[0];
            y = min[1] + random::uniform() * diff[1];

            if (not occupancy_map->isFree(Vector3d(x, y, 0.0)))
                continue;

            a = random::uniform() * 2 * M_PI - M_PI;
        }

        Pose2D p(x, y , a);
        VectorXd residuals;
        MatchSurface2D match_surface(distance_map, surface, p.state);

        match_surface.eval(residuals, 0);

        double error = residuals.squaredNorm();
        if ( error < best_error ){
            best_error = error;
            pose_ = p;
        }
    } // end for

}

lama::Loc2D::StrategyPtr lama::Loc2D::makeStrategy(const std::string& name, const VectorXd& parameters)
{
    if (name == "lm")
        return StrategyPtr(new LevenbergMarquard);

    return StrategyPtr(new GaussNewton);
}

lama::Loc2D::RobustCostPtr lama::Loc2D::makeRobust(const std::string& name, const double& param)
{
    if (name == "cauchy")
        return RobustCostPtr(new CauchyWeight(0.15));
    else if (name == "tstudent")
        return RobustCostPtr(new TDistributionWeight(3));
    else if (name == "tukey")
        return RobustCostPtr(new TukeyWeight);
    else
        return RobustCostPtr(new UnitWeight);
}


