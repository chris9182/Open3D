// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2018-2021 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4267)
#endif

#include "open3d/geometry/KDTreeFlann.h"

#include <nanoflann.hpp>

#include "open3d/geometry/HalfEdgeTriangleMesh.h"
#include "open3d/geometry/PointCloud.h"
#include "open3d/geometry/TriangleMesh.h"
#include "open3d/utility/Logging.h"

namespace open3d {
namespace geometry {

KDTreeFlann::KDTreeFlann() {}

KDTreeFlann::KDTreeFlann(const Eigen::MatrixXd &data) { SetMatrixData(data); }

KDTreeFlann::KDTreeFlann(const Geometry &geometry) { SetGeometry(geometry); }

KDTreeFlann::KDTreeFlann(const pipelines::registration::Feature &feature) {
    SetFeature(feature);
}

KDTreeFlann::~KDTreeFlann() {}

bool KDTreeFlann::SetMatrixData(const Eigen::MatrixXd &data) {
    return SetRawData(Eigen::Map<const Eigen::MatrixXd>(
            data.data(), data.rows(), data.cols()));
}

bool KDTreeFlann::SetGeometry(const Geometry &geometry) {
    switch (geometry.GetGeometryType()) {
        case Geometry::GeometryType::PointCloud:
            return SetRawData(Eigen::Map<const Eigen::MatrixXd>(
                    (const double *)((const PointCloud &)geometry)
                            .points_.data(),
                    3, ((const PointCloud &)geometry).points_.size()));
        case Geometry::GeometryType::TriangleMesh:
        case Geometry::GeometryType::HalfEdgeTriangleMesh:
            return SetRawData(Eigen::Map<const Eigen::MatrixXd>(
                    (const double *)((const TriangleMesh &)geometry)
                            .vertices_.data(),
                    3, ((const TriangleMesh &)geometry).vertices_.size()));
        case Geometry::GeometryType::Image:
        case Geometry::GeometryType::Unspecified:
        default:
            utility::LogWarning(
                    "[KDTreeFlann::SetGeometry] Unsupported Geometry type.");
            return false;
    }
}

bool KDTreeFlann::SetFeature(const pipelines::registration::Feature &feature) {
    return SetMatrixData(feature.data_);
}

template <typename T>
int KDTreeFlann::Search(const T &query,
                        const KDTreeSearchParam &param,
                        std::vector<int> &indices,
                        std::vector<double> &distance2) const {
    switch (param.GetSearchType()) {
        case KDTreeSearchParam::SearchType::Knn:
            return SearchKNN(query, ((const KDTreeSearchParamKNN &)param).knn_,
                             indices, distance2);
        case KDTreeSearchParam::SearchType::Radius:
            return SearchRadius(
                    query, ((const KDTreeSearchParamRadius &)param).radius_,
                    indices, distance2);
        case KDTreeSearchParam::SearchType::Hybrid:
            return SearchHybrid(
                    query, ((const KDTreeSearchParamHybrid &)param).radius_,
                    ((const KDTreeSearchParamHybrid &)param).max_nn_, indices,
                    distance2);
        case KDTreeSearchParam::SearchType::NNChain:
            return SearchNNChain(
                    query, ((const KDTreeSearchParamNNChain &)param).radiusLocal_,
                    ((const KDTreeSearchParamNNChain &)param).chainLength_, indices,
                    distance2);
        default:
            return -1;
    }
    return -1;
}

template <typename T>
int KDTreeFlann::SearchKNN(const T &query,
                           int knn,
                           std::vector<int> &indices,
                           std::vector<double> &distance2) const {
    // This is optimized code for heavily repeated search.
    // Other flann::Index::knnSearch() implementations lose performance due to
    // memory allocation/deallocation.
    if (data_.empty() || dataset_size_ <= 0 ||
        size_t(query.rows()) != dimension_ || knn < 0) {
        return -1;
    }
    indices.resize(knn);
    distance2.resize(knn);
    std::vector<Eigen::Index> indices_eigen(knn);
    int k = nanoflann_index_->index->knnSearch(
            query.data(), knn, indices_eigen.data(), distance2.data());
    indices.resize(k);
    distance2.resize(k);
    std::copy_n(indices_eigen.begin(), k, indices.begin());
    return k;
}

template <typename T>
int KDTreeFlann::SearchRadius(const T &query,
                              double radius,
                              std::vector<int> &indices,
                              std::vector<double> &distance2) const {
    // This is optimized code for heavily repeated search.
    // Since max_nn is not given, we let flann to do its own memory management.
    // Other flann::Index::radiusSearch() implementations lose performance due
    // to memory management and CPU caching.
    if (data_.empty() || dataset_size_ <= 0 ||
        size_t(query.rows()) != dimension_) {
        return -1;
    }
    std::vector<std::pair<Eigen::Index, double>> indices_dists;
    int k = nanoflann_index_->index->radiusSearch(
            query.data(), radius * radius, indices_dists,
            nanoflann::SearchParams(-1, 0.0));
    indices.resize(k);
    distance2.resize(k);
    for (int i = 0; i < k; ++i) {
        indices[i] = indices_dists[i].first;
        distance2[i] = indices_dists[i].second;
    }
    return k;
}

template <typename T>
int KDTreeFlann::SearchHybrid(const T &query,
                              double radius,
                              int max_nn,
                              std::vector<int> &indices,
                              std::vector<double> &distance2) const {
    // This is optimized code for heavily repeated search.
    // It is also the recommended setting for search.
    // Other flann::Index::radiusSearch() implementations lose performance due
    // to memory allocation/deallocation.
    if (data_.empty() || dataset_size_ <= 0 ||
        size_t(query.rows()) != dimension_ || max_nn < 0) {
        return -1;
    }
    distance2.resize(max_nn);
    std::vector<Eigen::Index> indices_eigen(max_nn);
    int k = nanoflann_index_->index->knnSearch(
            query.data(), max_nn, indices_eigen.data(), distance2.data());
    k = std::distance(distance2.begin(),
                      std::lower_bound(distance2.begin(), distance2.begin() + k,
                                       radius * radius));
    indices.resize(k);
    distance2.resize(k);
    std::copy_n(indices_eigen.begin(), k, indices.begin());
    return k;
}

template <typename T>
int KDTreeFlann::SearchNNChain(const T &query,
                              double radiusLocal,
                              int chainLength,
                              std::vector<int> &indices,
                              std::vector<double> &distance2) const {

    // This is optimized code for heavily repeated search.
    // Since max_nn is not given, we let flann to do its own memory management.
    // Other flann::Index::radiusSearch() implementations lose performance due
    // to memory management and CPU caching.
    if (data_.empty() || dataset_size_ <= 0 ||
        size_t(query.rows()) != dimension_) {
        return -1;
    }
    double* initial=(double *)query.data();
    flann::Matrix<double> query_flann(initial, 1, dimension_);
    flann::SearchParams param(-1, 0.0);
    param.max_neighbors = -1;
    std::vector<std::vector<int>> total_indices_vec(1);
    std::vector<std::vector<double>> total_dists_vec(1);
    flann_index_->radiusSearch(query_flann, total_indices_vec, total_dists_vec,
                                              float(radiusLocal * radiusLocal*chainLength*chainLength), param);

    std::vector<int> validIndices(1);
    std::vector<int> next_indices_vec(1);

    for(int i=0;i<chainLength;++i) {
        double *nextData;
        int vNum = 0;

        if (i<1) {
            vNum=1;
            nextData=new double[3];
            nextData[0]=initial[0];
            nextData[1]=initial[1];
            nextData[2]=initial[2];
        } else {
            if(next_indices_vec.size()<1)break;
            nextData = new double[next_indices_vec.size() * 3];
            for (int ind : next_indices_vec) {
                int vInd = vNum * 3;
                int baseInd = ind * 3;
                nextData[vInd] = data_[baseInd + 0];
                nextData[vInd + 1] = data_[baseInd + 1];
                nextData[vInd + 2] = data_[baseInd + 2];
                ++vNum;
            }
        }
        flann::Matrix<double> query_flannNext(nextData, vNum, dimension_);
        std::vector<std::vector<int>> indices_vec(1);
        std::vector<std::vector<double>> dists_vec(1);

        flann_index_->radiusSearch(query_flannNext, indices_vec, dists_vec,
                                           float(radiusLocal * radiusLocal), param);

        next_indices_vec.clear();
        for(std::vector<int> vec:indices_vec)
            for(int ind:vec)
                if (std::find(validIndices.begin(),
                              validIndices.end(),
                              ind) == validIndices.end()){
                    validIndices.push_back(ind);
                    next_indices_vec.push_back(ind);
                }
        delete [] nextData;
    }

    std::vector<int> result_indices_vec(1);
    std::vector<double> result_dists_vec(1);

    for(int ind:validIndices){
        auto it=std::find(total_indices_vec[0].begin(),
                          total_indices_vec[0].end(),
                          ind);
        if (it != total_indices_vec[0].end()){
            int pos = it - total_indices_vec[0].begin();
            result_indices_vec.push_back(total_indices_vec[0][pos]);
            result_dists_vec.push_back(total_dists_vec[0][pos]);
        }
    }

    indices = result_indices_vec;
    distance2 = result_dists_vec;
    return result_indices_vec.size();
}

bool KDTreeFlann::SetRawData(const Eigen::Map<const Eigen::MatrixXd> &data) {
    dimension_ = data.rows();
    dataset_size_ = data.cols();
    if (dimension_ == 0 || dataset_size_ == 0) {
        utility::LogWarning("[KDTreeFlann::SetRawData] Failed due to no data.");
        return false;
    }
    data_.resize(dataset_size_ * dimension_);
    memcpy(data_.data(), data.data(),
           dataset_size_ * dimension_ * sizeof(double));
    data_interface_.reset(new Eigen::Map<const Eigen::MatrixXd>(data));
    nanoflann_index_.reset(
            new KDTree_t(dimension_, std::cref(*data_interface_), 15));
    nanoflann_index_->index->buildIndex();
    return true;
}

template int KDTreeFlann::Search<Eigen::Vector3d>(
        const Eigen::Vector3d &query,
        const KDTreeSearchParam &param,
        std::vector<int> &indices,
        std::vector<double> &distance2) const;
template int KDTreeFlann::SearchKNN<Eigen::Vector3d>(
        const Eigen::Vector3d &query,
        int knn,
        std::vector<int> &indices,
        std::vector<double> &distance2) const;
template int KDTreeFlann::SearchRadius<Eigen::Vector3d>(
        const Eigen::Vector3d &query,
        double radius,
        std::vector<int> &indices,
        std::vector<double> &distance2) const;
template int KDTreeFlann::SearchHybrid<Eigen::Vector3d>(
        const Eigen::Vector3d &query,
        double radius,
        int max_nn,
        std::vector<int> &indices,
        std::vector<double> &distance2) const;
template int KDTreeFlann::SearchNNChain<Eigen::Vector3d>(
        const Eigen::Vector3d &query,
        double radiusLocal,
        int chainLength,
        std::vector<int> &indices,
        std::vector<double> &distance2) const;
        

template int KDTreeFlann::Search<Eigen::VectorXd>(
        const Eigen::VectorXd &query,
        const KDTreeSearchParam &param,
        std::vector<int> &indices,
        std::vector<double> &distance2) const;
template int KDTreeFlann::SearchKNN<Eigen::VectorXd>(
        const Eigen::VectorXd &query,
        int knn,
        std::vector<int> &indices,
        std::vector<double> &distance2) const;
template int KDTreeFlann::SearchRadius<Eigen::VectorXd>(
        const Eigen::VectorXd &query,
        double radius,
        std::vector<int> &indices,
        std::vector<double> &distance2) const;
template int KDTreeFlann::SearchHybrid<Eigen::VectorXd>(
        const Eigen::VectorXd &query,
        double radius,
        int max_nn,
        std::vector<int> &indices,
        std::vector<double> &distance2) const;
template int KDTreeFlann::SearchNNChain<Eigen::VectorXd>(
        const Eigen::VectorXd &query,
        double radiusLocal,
        int chainLength,
        std::vector<int> &indices,
        std::vector<double> &distance2) const;

}  // namespace geometry
}  // namespace open3d

#ifdef _MSC_VER
#pragma warning(pop)
#endif
