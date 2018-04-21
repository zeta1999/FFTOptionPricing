#ifndef OPTIONCALIBRATION_H
#define OPTIONCALIBRATION_H
#include <complex>
#include <vector>
#include <algorithm>
#include "FunctionalUtilities.h"
#include "fft.h"
#include <tuple>
#include "monotone_spline.h"

/** 
 * Optimization specific
 * to Levy processes and
 * the inverse problem for 
 * option prices.
 * Based off the following 
 * work:
 * Option calibration of 
 * exponential Levy models
 * by Belomestny and ReiB
 * and the updates from Sohl
 * and Trabs
 * */
namespace optioncal{

    template<typename T>
    auto maxZeroOrNumber(const T& num){
        return num>0.0?num:T(0.0);
    }
    constexpr auto cmp=std::complex<double>(0, 1);

    auto getDU(int N, double uMax){
        return 2.0*uMax/N;
    }
    auto getDX(int N, double xMin,double xMax){
        return (xMax-xMin)/(N-1);
    }
    auto getUMax(int N, double xMin, double xMax){
        return M_PI*(N-1)/(xMax-xMin);
    }
    
    template<typename ArrayU, typename Fn, typename ApplyEachSum, typename T>
    auto DFT(const std::vector<ArrayU>& uArray, Fn&& fn, ApplyEachSum&& fnU, const T& xMin, const T& xMax, int N){
        T dx=(xMax-xMin)/(N-1);
        int uSize=uArray.size();
        return futilities::for_each_parallel(0, uSize, [&](const auto& uIndex){
            auto u=uArray[uIndex];
            return fnU(u, futilities::sum(0, N, [&](const auto& index){
                auto simpson=(index==0||index==(N-1))?1.0:(index%2==0?2.0:4.0);
                auto x=xMin+dx*index;
                return exp(cmp*u*x)*fn(u, x, index)*simpson*dx/3.0;
            }));
        });
        
    }

    template<typename T>
    auto transformPrice(const T& p, const T& v){
        return p/v;
    }

    template<typename Stock, typename V, typename FN>
    auto transformPrices(const std::vector<V>& arr, const Stock& asset, const V& minV, const V& maxV, FN&& transform){
        const int arrLength=arr.size();
        std::vector<V> paddedArr(arrLength+2);
        paddedArr=futilities::for_each_parallel_subset(std::move(paddedArr), 1, 1, [&](const auto& v, const auto& index){
            return transform(arr[index-1], asset, index);
        });
        paddedArr.front()=transform(minV, asset, 0);
        paddedArr.back()=transform(maxV, asset, arrLength);
        return std::move(paddedArr);
    }
    template<typename Stock, typename V>
    auto transformPrices(const std::vector<V>& arr, const Stock& asset, const V& minV, const V& maxV){
        return transformPrices(arr, asset, minV, maxV, [](const auto& p, const auto& v, const auto& index){
           return transformPrice(p, v);
        });
    }

    template<typename Arr, typename FN1, typename FN2, typename FN3>
    auto filter(const Arr& arr, FN1&& cmp, FN2&& fv1, FN3&& fv2){
        Arr v1;
        Arr v2;
        for(int i=0; i<arr.size();++i){
            const auto x=arr[i];
            if(cmp(x, i)){
                v1.emplace_back(fv1(x, i));
            }else{
                v2.emplace_back(fv2(x, i));
            }
        }
        return std::make_tuple(v1, v2);
    }

    template< typename Strike, typename MarketPrice, typename AssetPrice, typename Discount>
    auto getOptionSpline(const std::vector<Strike>& strikes, const std::vector<MarketPrice>& options,  const AssetPrice& stock, const Discount& discount, const Strike& minStrike, const Strike& maxStrike){
        auto paddedStrikes=transformPrices(strikes, stock, minStrike, maxStrike);
        const auto minOption=stock-minStrike*discount;
        const auto maxOption=0.0000001;
        
        auto optionPrices=transformPrices(options, stock, minOption, maxOption);
       
        const auto threshold=.95;//this is rather arbitrary
   
        auto filteredStrikes=filter(paddedStrikes, [&](const auto& x, const auto& i){
            return x<threshold;
        }, [&](const auto& x, const auto& i){
            return x;
        }, [&](const auto& x, const auto& i){
            return x;
        });

        auto filteredPrices=filter(optionPrices, [&](const auto& x, const auto& i){
            return paddedStrikes[i]<threshold;
        }, [&](const auto& x, const auto& i){
            return x-maxZeroOrNumber(1-paddedStrikes[i]*discount);
        }, [&](const auto& x, const auto& i){
            return log(x);
        });
        auto sLow=spline::generateSpline(std::move(std::get<0>(filteredStrikes)), std::move(std::get<0>(filteredPrices)));
        auto sHigh=spline::generateSpline(std::move(std::get<1>(filteredStrikes)), std::move(std::get<1>(filteredPrices)));

        return [sLow=std::move(sLow), sHigh=std::move(sHigh), discount=std::move(discount), threshold=std::move(threshold)](const auto& k){
            if(k<threshold){
                return sLow(k);
            }
            else {
                return exp(sHigh(k))-maxZeroOrNumber(1-k*discount);
            }
        };
        
    }


    template<typename Strike, typename MarketPrice, typename AssetPrice, typename R, typename T>
    auto generateFOEstimate(const std::vector<Strike>& strikes, const std::vector<MarketPrice>& options,  const AssetPrice& stock, const R& r, const T& t, const Strike& minStrike, const Strike& maxStrike){
        auto discount=exp(-r*t);
        auto s=getOptionSpline(strikes, options, stock, discount, minStrike, maxStrike);
        return [
            spline=std::move(s), 
            minStrike=transformPrice(minStrike, stock), 
            maxStrike=transformPrice(maxStrike, stock),
            discount=std::move(discount),
            t=std::move(t)
        ](int N, const auto& uArray){
            const auto xMin=log(discount*minStrike);
            const auto xMax=log(discount*maxStrike);
            auto valOrZero=[](const auto& v){
                return v>0.0?v:0.0;
            };
            return DFT(uArray, [&](const auto& u, const auto& x, const auto& index){
                const auto expX=exp(x);
                const auto strike=expX/discount;
                const auto offset=1.0-exp(x);
                const auto optionPrice=spline(strike);
                return valOrZero(optionPrice);
            }, [&](const auto& u, const auto& value){
                const auto front=u*cmp*(1.0+u*cmp);
                return log(1.0+front*value);
            }, xMin, xMax, N);
        };   

    }
    
    template<typename PhiHat, typename LogCfFN, typename DiscreteU>
    auto getObjFn_arr(PhiHat&& phiHattmp, LogCfFN&& cfFntmp, DiscreteU&& uArraytmp){
        return [
            phiHat=std::move(phiHattmp), 
            cfFn=std::move(cfFntmp), 
            uArray=std::move(uArraytmp)
        ](const auto& arrayParam){
            return futilities::sum(uArray, [&](const auto& u, const auto& index){
                return std::norm(
                    phiHat[index]-cfFn(std::complex<double>(1.0, u), arrayParam)
                )/(double)uArray.size();         
            });
        };
    }

}

#endif