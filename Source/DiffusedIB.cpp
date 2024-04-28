
#include <DiffusedIB.H>

#include <AMReX_ParmParse.H>
#include <AMReX_TagBox.H>
#include <AMReX_Utility.H>
#include <AMReX_PhysBCFunct.H>
#include <AMReX_MLNodeLaplacian.H>
#include <AMReX_FillPatchUtil.H>
#include <iamr_constants.H>

#include <filesystem>
namespace fs = std::filesystem;

#define GHOST_CELLS 2

using namespace amrex;

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                     global variable                           */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
#define LOCAL_LEVEL 0
namespace ParticleProperties{
    Vector<Real> _x{}, _y{}, _z{}, _rho{};
    Vector<Real> Vx{}, Vy{}, Vz{};
    Real _radius;
    Vector<int> TLX{}, TLY{},TLZ{},RLX{},RLY{},RLZ{};
    int euler_finest_level{0};
    int euler_velocity_index{0};
    int euler_force_index{0};
    Real euler_fluid_rho{0.0};
    int verbose{0};
    int loop_time{0};

    GpuArray<Real, 3> plo{0.0,0.0,0.0}, phi{0.0,0.0,0.0}, dx{0.0, 0.0, 0.0};
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                     other function                            */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

void nodal_phi_to_pvf(MultiFab& pvf, const MultiFab& phi_nodal)
{

    amrex::Print() << "In the nodal_phi_to_pvf\n";

#ifdef AMREX_USE_OMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
    for (MFIter mfi(pvf,TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {
        const Box& bx = mfi.tilebox();
        auto const& pvffab   = pvf.array(mfi);
        auto const& pnfab = phi_nodal.array(mfi);
        amrex::ParallelFor(bx, [pvffab, pnfab]
        AMREX_GPU_DEVICE(int i, int j, int k) noexcept
        {
            Real num = 0.0;
            for(int kk=k; kk<=k+1; kk++) {
                for(int jj=j; jj<=j+1; jj++) {
                    for(int ii=i; ii<=i+1; ii++) {
                        num += (-pnfab(ii,jj,kk)) * nodal_phi_to_heavi(-pnfab(ii,jj,kk));
                    }
                }
            }
            Real deo = 0.0;
            for(int kk=k; kk<=k+1; kk++) {
                for(int jj=j; jj<=j+1; jj++) {
                    for(int ii=i; ii<=i+1; ii++) {
                        deo += std::abs(pnfab(ii,jj,kk));
                    }
                }
            }
            pvffab(i,j,k) = num / (deo + 1.e-12);
        });
    }

}

void calculate_phi_nodal(MultiFab& phi_nodal, kernel& current_kernel)
{
    phi_nodal.setVal(0.0);

    Real Xp2 = Math::powi<2>(current_kernel.location[0]);
    Real Yp2 = Math::powi<2>(current_kernel.location[1]);
    Real Zp2 = Math::powi<2>(current_kernel.location[2]);
    Real Rp2 = Math::powi<2>(current_kernel.radius);

    for (MFIter mfi(phi_nodal,TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {
        const Box& bx = mfi.tilebox();
        auto const& pnfab = phi_nodal.array(mfi);
        const auto *pnfab_ptr = &pnfab;
        auto *dx = &ParticleProperties::dx;
        auto *plo = &ParticleProperties::plo;
        amrex::ParallelFor(bx, [=]
            AMREX_GPU_DEVICE(int i, int j, int k) noexcept
            {
                Real Xn = i * (*dx)[0] + (*plo)[0];
                Real Yn = j * (*dx)[1] + (*plo)[1];
                Real Zn = k * (*dx)[2] + (*plo)[2];

                (*pnfab_ptr)(i,j,k) = std::sqrt((Math::powi<2>(Xn) - Xp2)/Rp2 
                                    + (Math::powi<2>(Yn) - Yp2)/Rp2 
                                    + (Math::powi<2>(Zn) - Zp2)/Rp2
                                    ) - 1.0;
            }
        );
    }
}

void CalculateSum_cir (RealVect& sum,
                       MultiFab& E_old,
                       MultiFab& E,
                       const MultiFab& pvf,
                       int EulerVelIndex)
{
    const Real d = Math::powi<3>(ParticleProperties::dx[0]);

    for (MFIter mfi(E,TilingIfNotGPU()); mfi.isValid(); ++mfi)
    {
        const Box& bx = mfi.tilebox();
        auto const& uarray_old = E_old.array(mfi);
        auto const& uarray = E.array(mfi);
        auto const& pvffab = pvf.array(mfi);
        const auto * old_ptr = &uarray_old;
        const auto * new_ptr = &uarray;
        const auto * pvf_ptr = &pvffab;
        auto* sum_ptr = &sum;
        amrex::ParallelFor(bx, [=]
            AMREX_GPU_DEVICE(int i, int j, int k) noexcept
            {
                Gpu::Atomic::AddNoRet(&(*sum_ptr)[0], ((*new_ptr)(i, j, k, EulerVelIndex    ) - (*old_ptr)(i, j, k, EulerVelIndex    )) * d * (*pvf_ptr)(i, j, k));
                Gpu::Atomic::AddNoRet(&(*sum_ptr)[1], ((*new_ptr)(i, j, k, EulerVelIndex + 1) - (*old_ptr)(i, j, k, EulerVelIndex + 1)) * d * (*pvf_ptr)(i, j, k));
                Gpu::Atomic::AddNoRet(&(*sum_ptr)[2], ((*new_ptr)(i, j, k, EulerVelIndex + 2) - (*old_ptr)(i, j, k, EulerVelIndex + 2)) * d * (*pvf_ptr)(i, j, k));
            }
        );
    }

}

[[nodiscard]] AMREX_FORCE_INLINE
Real cal_momentum(Real rho, Real radius)
{
    return 8.0 * Math::pi<Real>() * rho * Math::powi<5>(radius) / 15.0;
}

AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
void deltaFunction(Real xf, Real xp, Real h, Real& value, DELTA_FUNCTION_TYPE type)
{
    Real rr = amrex::Math::abs(( xf - xp ) / h);

    switch (type) {
    case DELTA_FUNCTION_TYPE::FOUR_POINT_IB:
        if(rr >= 0 && rr < 1 ){
            value = 1.0 / 8.0 * ( 3.0 - 2.0 * rr + std::sqrt( 1.0 + 4.0 * rr - 4 * Math::powi<2>(rr))) / h;
        }else if (rr >= 1 && rr < 2) {
            value = 1.0 / 8.0 * ( 5.0 - 2.0 * rr - std::sqrt( -7.0 + 12.0 * rr - 4 * Math::powi<2>(rr))) / h;
        }else {
            value = 0;
        }
        break;
    case DELTA_FUNCTION_TYPE::THREE_POINT_IB:
        if(rr >= 0.5 && rr < 1.5){
            value = 1.0 / 6.0 * ( 5.0 - 3.0 * rr - std::sqrt( - 3.0 * Math::powi<2>( 1 - rr) + 1.0 )) / h;
        }else if (rr >= 0 && rr < 0.5) {
            value = 1.0 / 3.0 * ( 1.0 + std::sqrt( 1.0 - 3 * Math::powi<2>(rr))) / h;
        }else {
            value = 0;
        }
        break;
    default:
        break;
    }
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                    mParticle member function                  */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
//loop all particels
void mParticle::InteractWithEuler(int iStep, 
                                  amrex::Real time, 
                                  MultiFab &EulerVel, 
                                  MultiFab &EulerForce, 
                                  Real dt,
                                  DELTA_FUNCTION_TYPE type){

    if (verbose) amrex::Print() << "[Particle] mParticle::InteractWithEuler\n";

    for(kernel& kernel : particle_kernels){
        InitialWithLargrangianPoints(kernel); // Initialize markers for a specific particle
        //UpdateMarkers(kernel, dt);
        
        amrex::Real ib_force_x = 0.0;
        amrex::Real ib_force_y = 0.0;
        amrex::Real ib_force_z = 0.0;
        
        //for 1 -> Ns
        int loop = ParticleProperties::loop_time;
        BL_ASSERT(loop > 0);
        while(loop > 0){
            if(verbose) amrex::Print() << "[Particle] Ns loop index : " << loop << "\n";
            
            VelocityInterpolation(EulerVel, type);
            ComputeLagrangianForce(dt, kernel);
            
            EulerForce.setVal(0.0, ParticleProperties::euler_force_index, 3, GHOST_CELLS); // clear Euler force
            kernel.ib_force.scale(0); // clear kernel ib_force
            ForceSpreading(EulerForce, kernel.ib_force, kernel.dv, type);

            amrex::ParallelAllReduce::Sum(&kernel.ib_force[0], 3, ParallelDescriptor::Communicator());

            ib_force_x += kernel.ib_force[0];
            ib_force_y += kernel.ib_force[1];
            ib_force_z += kernel.ib_force[2];            

            VelocityCorrection(EulerVel, EulerForce, dt);
            
            loop--;
        }
        
        kernel.ib_force[0] = ib_force_x;
        kernel.ib_force[1] = ib_force_y;
        kernel.ib_force[2] = ib_force_z;  
        WriteIBForce(iStep, time, kernel);
    }
}

void mParticle::InitParticles(const Vector<Real>& x,
                              const Vector<Real>& y,
                              const Vector<Real>& z,
                              const Vector<Real>& rho_s,
                              const Vector<Real>& Vx,
                              const Vector<Real>& Vy,
                              const Vector<Real>& Vz,
                              const Vector<int>& TLXt,
                              const Vector<int>& TLYt,
                              const Vector<int>& TLZt,
                              const Vector<int>& RLXt,
                              const Vector<int>& RLYt,
                              const Vector<int>& RLZt,
                              Real radius,
                              Real gravity,
                              int _verbose)
{
    verbose = _verbose;
    if (verbose) amrex::Print() << "[Particle] mParticle::InitParticles\n";

    m_gravity[2] = gravity;

    //pre judge
    if(!((x.size() == y.size()) && (x.size() == z.size()))){
        Print() << "particle's position container are all different size";
        return;
    }
    //all the particles have same radius
    Real h = m_gdb->Geom(LOCAL_LEVEL).CellSizeArray()[0];
    int Ml = static_cast<int>( Math::pi<Real>() / 3 * (12 * Math::powi<2>(radius / h)));
    Real dv = Math::pi<Real>() * h / 3 / Ml * (12 * radius * radius + h * h);

    if (verbose) amrex::Print() << "h: " << h << ", Ml: " << Ml << ", dv: " << dv << "\n"
                                << "gravity : " << gravity << "\n";

    for(int index = 0; index < x.size(); index++){
        kernel mKernel;
        mKernel.id = index + 1;
        mKernel.location[0] = x[index];
        mKernel.location[1] = y[index];
        mKernel.location[2] = z[index];
        mKernel.velocity[0] = Vx[index];
        mKernel.velocity[1] = Vy[index];
        mKernel.velocity[2] = Vz[index];
        mKernel.TLX = TLXt[index];
        mKernel.TLY = TLYt[index];
        mKernel.TLZ = TLZt[index];
        mKernel.RLX = RLXt[index];
        mKernel.RLY = RLYt[index];
        mKernel.RLZ = RLZt[index];
        mKernel.rho = rho_s[index];
        mKernel.radius = radius;
        mKernel.ml = Ml;
        mKernel.dv = dv;

        particle_kernels.emplace_back(mKernel);

        if (verbose) amrex::Print() << "Kernel " << index << ": Location (" << x[index] << ", " << y[index] << ", " << z[index] 
                                    << "), Velocity : (" << mKernel.velocity[0] << ", " << mKernel.velocity[1] << ", "<< mKernel.velocity[2] 
                                    << "), Radius: " << radius << ", Ml: " << Ml << ", dv: " << dv << ", Rho: " << mKernel.rho << "\n";
    }
    //get particle tile
    std::pair<int, int> key{0,0};
    auto& particleTileTmp = GetParticles(LOCAL_LEVEL)[key];

    //insert markers
    if ( ParallelDescriptor::MyProc() == ParallelDescriptor::IOProcessorNumber() ) {
        //insert particle's markers

        Real phiK = 0;
        for(int marker_index = 0; marker_index < Ml; marker_index++){
            //insert code
            ParticleType markerP;
            markerP.id() = ParticleType::NextID();
            markerP.cpu() = ParallelDescriptor::MyProc();
            markerP.pos(0) = 0.01;
            markerP.pos(1) = 0.01;
            markerP.pos(2) = 0.01;

            std::array<ParticleReal, numAttri> Marker_attr;
            Marker_attr[U_Marker] = 0.0;
            Marker_attr[V_Marker] = 0.0;
            Marker_attr[W_Marker] = 0.0;
            Marker_attr[Fx_Marker] = 0.0;
            Marker_attr[Fy_Marker] = 0.0;
            Marker_attr[Fz_Marker] = 0.0;

            Real Hk = -1.0 + 2.0 * (marker_index) / ( Ml - 1.0);
            Real thetaK = std::acos(Hk);    
            if(marker_index == 0 || marker_index == (Ml - 1)){
                phiK = 0;
            }else {
                phiK = std::fmod( phiK + 3.809 / std::sqrt(Ml) / std::sqrt( 1 - Math::powi<2>(Hk)) , 2 * Math::pi<Real>());
            }
            Marker_attr[P_thetaK] = thetaK;
            Marker_attr[P_phiK] = phiK;

            particleTileTmp.push_back(markerP);
            particleTileTmp.push_back_real(Marker_attr);
        }
    }
    Redistribute(); // Still needs to redistribute here! 
    if (verbose) WriteAsciiFile(amrex::Concatenate("particle", 0));
}

AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
void InitalLargrangianPointsLoc (amrex::ParticleReal& x,  amrex::ParticleReal& y, amrex::ParticleReal& z,
                                 const amrex::Real thetaK,
                                 const amrex::Real phiK,
                                 const amrex::RealVect location,
                                 const amrex::Real radius,
                                 const int ml) noexcept
{
    // update LargrangianPoint position with particle position           
    x = location[0] + radius * std::sin(thetaK) * std::cos(phiK);
    y = location[1] + radius * std::sin(thetaK) * std::sin(phiK);
    z = location[2] + radius * std::cos(thetaK);
}

void mParticle::InitialWithLargrangianPoints(const kernel& current_kernel){

    if (verbose) amrex::Print() << "mParticle::InitialWithLargrangianPoints\n";
    // Update the markers' locations
    for(mParIter pti(*this, LOCAL_LEVEL); pti.isValid(); ++pti){

        auto *particles = pti.GetArrayOfStructs().data();
        auto& attri = pti.GetAttribs();
        auto* thetaK_ptr = attri[P_ATTR::P_thetaK].data();
        auto* phiK_ptr = attri[P_ATTR::P_phiK].data();
        const Long np = pti.numParticles();

        const auto location = current_kernel.location;
        const auto radius = current_kernel.radius;
        const auto ml = current_kernel.ml;

        amrex::ParallelFor( np, [=]
            AMREX_GPU_DEVICE (int i) noexcept {
                InitalLargrangianPointsLoc(particles[i].pos(0),particles[i].pos(1),particles[i].pos(2),
                                           thetaK_ptr[i],
                                           phiK_ptr[i],    
                                           location,
                                           radius,
                                           ml);
            }
        );
    }
    // Redistribute the markers after updating their locations
    Redistribute();
    if (verbose) WriteAsciiFile(amrex::Concatenate("particle", 1));
}

template <typename P = Particle<numAttri>>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
void VelocityInterpolation_cir(int p_iter, P const& p, Real& Up, Real& Vp, Real& Wp,
                               Array4<Real const> const& E, int EulerVIndex,
                               const int *lo, const int *hi, 
                               GpuArray<Real, AMREX_SPACEDIM> const& plo,
                               GpuArray<Real, AMREX_SPACEDIM> const& dx,
                               DELTA_FUNCTION_TYPE type)
{

    //std::cout << "lo " << lo[0] << " " << lo[1] << " "<< lo[2] << "\n";
    //std::cout << "hi " << hi[0] << " " << hi[1] << " "<< hi[2] << "\n";

    const Real d = AMREX_D_TERM(dx[0], *dx[1], *dx[2]);

    const Real lx = (p.pos(0) - plo[0]) / dx[0]; // x
    const Real ly = (p.pos(1) - plo[1]) / dx[1]; // y
    const Real lz = (p.pos(2) - plo[2]) / dx[2]; // z

    int i = static_cast<int>(Math::floor(lx)); // i
    int j = static_cast<int>(Math::floor(ly)); // j
    int k = static_cast<int>(Math::floor(lz)); // k

    //std::cout << "p_iter " << p_iter << " p.pos(0): " << p.pos(0) << " p.pos(1): " << p.pos(1) << " p.pos(2): " << p.pos(2) << "\n";

    // std::cout << "d: " << d << "\n"
    //         << "lx: " << lx << ", ly: " << ly << ", lz: " << lz << "\n"
    //         << "i: " << i << ", j: " << j << ", k: " << k << std::endl;

    Up = 0;
    Vp = 0;
    Wp = 0;
    //Euler to largrangian
    for(int ii = -2; ii < 3; ii++){
        for(int jj = -2; jj < 3; jj++){
            for(int kk = -2; kk < 3; kk ++){
                Real tU, tV, tW;
                const Real xi = (i + ii) * dx[0] + dx[0]/2;
                const Real yj = (j + jj) * dx[1] + dx[1]/2;
                const Real kz = (k + kk) * dx[2] + dx[2]/2;
                deltaFunction( p.pos(0), xi, dx[0], tU, type);
                deltaFunction( p.pos(1), yj, dx[1], tV, type);
                deltaFunction( p.pos(2), kz, dx[2], tW, type);
                const Real delta_value = tU * tV * tW;
                Up += delta_value * E(i + ii, j + jj, k + kk, EulerVIndex    ) * d;
                Vp += delta_value * E(i + ii, j + jj, k + kk, EulerVIndex + 1) * d;
                Wp += delta_value * E(i + ii, j + jj, k + kk, EulerVIndex + 2) * d;
            }
        }
    }
}

void mParticle::VelocityInterpolation(MultiFab &EulerVel,
                                      DELTA_FUNCTION_TYPE type)//
{

    if (verbose) amrex::Print() << "\tmParticle::VelocityInterpolation\n";

    //amrex::Print() << "euler_finest_level " << euler_finest_level << std::endl;
    const auto& gm = m_gdb->Geom(LOCAL_LEVEL);
    auto plo = gm.ProbLoArray();
    auto dx = gm.CellSizeArray();
    // attention
    // velocity ghost cells will be up-to-date
    EulerVel.FillBoundary(ParticleProperties::euler_velocity_index, 3, gm.periodicity());

    const int EulerVelocityIndex = ParticleProperties::euler_velocity_index;

    for(mParIter pti(*this, LOCAL_LEVEL); pti.isValid(); ++pti){
        
        const Box& box = pti.validbox();
        
        auto& particles = pti.GetArrayOfStructs();
        auto *p_ptr = particles.data();
        const Long np = pti.numParticles();

        auto& attri = pti.GetAttribs();
        auto* Up = attri[P_ATTR::U_Marker].data();
        auto* Vp = attri[P_ATTR::V_Marker].data();
        auto* Wp = attri[P_ATTR::W_Marker].data();
        const auto& E = EulerVel.array(pti);

        amrex::ParallelFor(np, [=] 
        AMREX_GPU_DEVICE (int i) noexcept{
            VelocityInterpolation_cir(i, p_ptr[i], Up[i], Vp[i], Wp[i], E, EulerVelocityIndex, box.loVect(), box.hiVect(), plo, dx, type);
        });
    }
    if (verbose) WriteAsciiFile(amrex::Concatenate("particle", 2));
    //amrex::Abort("stop here!");
}

template <typename P>
AMREX_GPU_HOST_DEVICE AMREX_FORCE_INLINE
void ForceSpreading_cic (P const& p,
                         ParticleReal fxP,
                         ParticleReal fyP,
                         ParticleReal fzP,
                         Real & ib_fx,
                         Real & ib_fy,
                         Real & ib_fz,
                         Array4<Real> const& E,
                         int EulerForceIndex,
                         Real dv,
                         GpuArray<Real,AMREX_SPACEDIM> const& plo,
                         GpuArray<Real,AMREX_SPACEDIM> const& dx,
                         DELTA_FUNCTION_TYPE type)
{
    //const Real d = AMREX_D_TERM(dx[0], *dx[1], *dx[2]);
    //plo to ii jj kk
    Real lx = (p.pos(0) - plo[0]) / dx[0];
    Real ly = (p.pos(1) - plo[1]) / dx[1];
    Real lz = (p.pos(2) - plo[2]) / dx[2];

    int i = static_cast<int>(Math::floor(lx));
    int j = static_cast<int>(Math::floor(ly));
    int k = static_cast<int>(Math::floor(lz));
    Real Fx = fxP * dv;
    Real Fy = fyP * dv;
    Real Fz = fzP * dv;
    Gpu::Atomic::AddNoRet(&ib_fx, Fx);
    Gpu::Atomic::AddNoRet(&ib_fy, Fy);
    Gpu::Atomic::AddNoRet(&ib_fz, Fz);
    //lagrangian to Euler
    for(int ii = -2; ii < +3; ii++){
        for(int jj = -2; jj < +3; jj++){
            for(int kk = -2; kk < +3; kk ++){
                Real tU, tV, tW;
                const Real xi = (i + ii) * dx[0] + dx[0]/2;
                const Real yj = (j + jj) * dx[1] + dx[1]/2;
                const Real kz = (k + kk) * dx[2] + dx[2]/2;
                deltaFunction( p.pos(0), xi, dx[0], tU, type);
                deltaFunction( p.pos(1), yj, dx[1], tV, type);
                deltaFunction( p.pos(2), kz, dx[2], tW, type);
                Real delta_value = tU * tV * tW;
                Gpu::Atomic::AddNoRet(&E(i + ii, j + jj, k + kk, EulerForceIndex  ), delta_value * Fx);
                Gpu::Atomic::AddNoRet(&E(i + ii, j + jj, k + kk, EulerForceIndex+1), delta_value * Fy);
                Gpu::Atomic::AddNoRet(&E(i + ii, j + jj, k + kk, EulerForceIndex+2), delta_value * Fz);
            }
        }
    }
}

void mParticle::ForceSpreading(MultiFab & EulerForce, 
                               RealVect& ib_force,
                               Real dv,
                               DELTA_FUNCTION_TYPE type){

    if (verbose) amrex::Print() << "\tmParticle::ForceSpreading\n";
    const auto& gm = m_gdb->Geom(LOCAL_LEVEL);
    auto plo = gm.ProbLoArray();
    auto dxi = gm.CellSizeArray();

    for(mParIter pti(*this, LOCAL_LEVEL); pti.isValid(); ++pti){
        const Long np = pti.numParticles();
        const auto& particles = pti.GetArrayOfStructs();
        auto Uarray = EulerForce[pti].array();
        auto& attri = pti.GetAttribs();

        auto *const fxP_ptr = attri[P_ATTR::Fx_Marker].data();
        auto *const fyP_ptr = attri[P_ATTR::Fy_Marker].data();
        auto *const fzP_ptr = attri[P_ATTR::Fz_Marker].data();
        const auto *const p_ptr = particles().data();

        auto* ib_ptr = &ib_force;
        auto force_index = ParticleProperties::euler_force_index;
        amrex::ParallelFor(np, [=]
        AMREX_GPU_DEVICE (int i) noexcept{       
            ForceSpreading_cic(p_ptr[i], fxP_ptr[i], fyP_ptr[i], fzP_ptr[i], (*ib_ptr)[0], (*ib_ptr)[1], (*ib_ptr)[2], Uarray, force_index, dv, plo, dxi, type);
        });
    }
    EulerForce.SumBoundary(ParticleProperties::euler_force_index, 3, gm.periodicity());

    if (false) {
        // Check the Multifab
        // Open a file stream for output
        std::ofstream outFile("EulerForce.txt");

        // Check the Multifab
        // for (MFIter mfi(EulerForce, TilingIfNotGPU()); mfi.isValid(); ++mfi)
        for (MFIter mfi(EulerForce, TilingIfNotGPU()); mfi.isValid(); ++mfi)
        {
            const Box& bx = mfi.validbox();
            outFile << "Box: " << bx << "\n"
                    << "From: (" << bx.smallEnd(0) << ", " << bx.smallEnd(1) << ", " << bx.smallEnd(2) << ") "
                    << "To: (" << bx.bigEnd(0) << ", " << bx.bigEnd(1) << ", " << bx.bigEnd(2) << ")\n";

            Array4<Real> const& a = EulerForce[mfi].array();

            // CPU context or illustrative purposes only
            for (int k = bx.smallEnd(2); k <= bx.bigEnd(2); ++k) {
                for (int j = bx.smallEnd(1); j <= bx.bigEnd(1); ++j) {
                    for (int i = bx.smallEnd(0); i <= bx.bigEnd(0); ++i) {
                        // This print statement is for demonstration and should not be used in actual GPU code.
                        outFile << "Processing i: " << i << ", j: " << j << ", k: " << k << " " << a(i,j,k,0) << " " << a(i,j,k,1) << " " << a(i,j,k,2) << "\n";
                    }
                }
            }
        }

        // Close the file when done
        outFile.close();
    }

}

void mParticle::UpdateMarkers(kernel& current_kernel, Real dt)
{
}

void mParticle::UpdateParticles(const MultiFab& Euler_old, 
                                const MultiFab& Euler, 
                                const MultiFab& pvf, 
                                kernel& kernel, Real dt)
{
    if (verbose) amrex::Print() << "mParticle::UpdateParticles\n";
    
    {//reduce all kernel data
        amrex::ParallelAllReduce::Sum(&kernel.sum_t[0], 3, ParallelDescriptor::Communicator());
        amrex::ParallelAllReduce::Sum(&kernel.sum_u[0], 3, ParallelDescriptor::Communicator());
        amrex::ParallelAllReduce::Sum(&kernel.ib_force[0], 3, ParallelDescriptor::Communicator());
    }
    //continue condition 6DOF
    if((kernel.TLX + kernel.TLY + kernel.TLZ + kernel.RLX + kernel.RLY + kernel.RLZ) == 0) return;
    //sum
    //CalculateSum_cir(kernel.sum_u, Euler_old, Euler, pvf, euler_velocity_index);


    //ioprocessor calculation 
    // if(amrex::ParallelDescriptor::MyProc() == ParallelDescriptor::IOProcessorNumber()){
        
        Real Vp = Math::pi<Real>() * 4 / 3 * Math::powi<3>(kernel.radius);
        //update the kernel's infomation and cal body force
        //update kernel velocity
        auto old_velocity = kernel.velocity;
        kernel.velocity = old_velocity
                        + (kernel.sum_u * ParticleProperties::euler_fluid_rho / dt 
                        - ParticleProperties::euler_fluid_rho * kernel.ib_force * kernel.dv
                        + m_gravity * (kernel.rho - ParticleProperties::euler_fluid_rho) * Vp
                        + kernel.Fcp) * dt / kernel.rho / Vp;
        //kernel.omega = ;
        kernel.velocity *= RealVect(kernel.TLX, kernel.TLY, kernel.TLZ);
        kernel.omega *= RealVect(kernel.RLX, kernel.RLY, kernel.RLZ);

        kernel.location += (kernel.velocity + old_velocity) * dt * 0.5;
    // }
    //Bcast velocity
    // ParallelDescriptor::Bcast(&kernel.location[0], 3, ParallelDescriptor::IOProcessorNumber());

    if (verbose) WriteAsciiFile(amrex::Concatenate("particle", 4));
}

void mParticle::ComputeLagrangianForce(Real dt, 
                                       const kernel& kernel)
{
    
    if (verbose) amrex::Print() << "\tmParticle::ComputeLagrangianForce\n";

    Real Ub = kernel.velocity[0];
    Real Vb = kernel.velocity[1];
    Real Wb = kernel.velocity[2];

    for(mParIter pti(*this, LOCAL_LEVEL); pti.isValid(); ++pti){
        const Long np = pti.numParticles();
        auto& attri = pti.GetAttribs();

        auto* Up = attri[P_ATTR::U_Marker].data();
        auto* Vp = attri[P_ATTR::V_Marker].data();
        auto* Wp = attri[P_ATTR::W_Marker].data();
        auto *FxP = attri[P_ATTR::Fx_Marker].data();
        auto *FyP = attri[P_ATTR::Fy_Marker].data();
        auto *FzP = attri[P_ATTR::Fz_Marker].data();

        amrex::ParallelFor(np,
        [=] AMREX_GPU_DEVICE (int i) noexcept{
            FxP[i] = (Ub - Up[i])/dt; //
            FyP[i] = (Vb - Vp[i])/dt; //
            FzP[i] = (Wb - Wp[i])/dt; //
        });
    }
    if (verbose) WriteAsciiFile(amrex::Concatenate("particle", 3));
}


void mParticle::VelocityCorrection(amrex::MultiFab &Euler, amrex::MultiFab &EulerForce, Real dt) const
{
    if(verbose) amrex::Print() << "\tmParticle::VelocityCorrection\n";
    MultiFab::Saxpy(Euler, dt, EulerForce, ParticleProperties::euler_force_index, ParticleProperties::euler_velocity_index, 3, 0); //VelocityCorrection
}

void mParticle::WriteParticleFile(int index)
{
    WriteAsciiFile(amrex::Concatenate("particle", index));
}

void mParticle::WriteIBForce(int step, amrex::Real time, kernel& current_kernel)
{
    
    if(amrex::ParallelDescriptor::MyProc() != ParallelDescriptor::IOProcessorNumber()) return; 

    std::string file("IB_Force_Particle_" + std::to_string(current_kernel.id) + ".csv");
    std::ofstream out_ib_force;

    std::string head;
    if(!fs::exists(file)){
        head = "iStep,time,X,Y,Z,Vx,Vy,Vz,OmegaX,OmegaY,OmegaZ,Fx,Fy,Fz\n";
    }else{
        head = "";
    }

    out_ib_force.open(file, std::ios::app);
    if(!out_ib_force.is_open()){
        amrex::Print() << "[Particle] write particle file error , step: " << step;
    }else{
        out_ib_force << head << step << "," << time << "," 
                     << current_kernel.location[0] << "," << current_kernel.location[1] << "," << current_kernel.location[2] << ","
                     << current_kernel.velocity[0] << "," << current_kernel.velocity[1] << "," << current_kernel.velocity[2] << ","
                     << current_kernel.omega[0] << "," << current_kernel.omega[1] << "," << current_kernel.omega[2] << ","
                     << current_kernel.ib_force[0] << "," << current_kernel.ib_force[1] << "," << current_kernel.ib_force[2] << "\n";
    }
    out_ib_force.close();
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/*                    Particles member function                  */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
void Particles::create_particles(const Geometry &gm,
                                 const DistributionMapping & dm,
                                 const BoxArray & ba)
{
    particle = new mParticle(gm, dm, ba);
    ParticleProperties::plo = gm.ProbLoArray();
    ParticleProperties::phi = gm.ProbHiArray();
    ParticleProperties::dx = gm.CellSizeArray();
}

mParticle* Particles::get_particles()
{
    return particle;
}


void Particles::init_particle(int level, Real gravity)
{  
    if(particle != nullptr){
        particle->InitParticles(
            ParticleProperties::_x, 
            ParticleProperties::_y, 
            ParticleProperties::_z,
            ParticleProperties::_rho,
            ParticleProperties::Vx,
            ParticleProperties::Vy,
            ParticleProperties::Vz,
            ParticleProperties::TLX,
            ParticleProperties::TLY,
            ParticleProperties::TLZ,
            ParticleProperties::RLX,
            ParticleProperties::RLY,
            ParticleProperties::RLZ,
            ParticleProperties::_radius,
            gravity,
            ParticleProperties::verbose);
    }
}

void Particles::Initialize()
{
    ParmParse pp("particle");

    std::string particle_inputfile;

    pp.get("input",particle_inputfile);
    
    if(!particle_inputfile.empty()){
        ParmParse p_file(particle_inputfile);
        p_file.getarr("x", ParticleProperties::_x);
        p_file.getarr("y", ParticleProperties::_y);
        p_file.getarr("z", ParticleProperties::_z);
        p_file.getarr("rho",ParticleProperties::_rho);
        p_file.get("radius", ParticleProperties::_radius);
        p_file.getarr("velocity_x", ParticleProperties::Vx);
        p_file.getarr("velocity_y", ParticleProperties::Vy);
        p_file.getarr("velocity_z", ParticleProperties::Vz);
        p_file.getarr("TLX", ParticleProperties::TLX);
        p_file.getarr("TLY", ParticleProperties::TLY);
        p_file.getarr("TLZ", ParticleProperties::TLZ);
        p_file.getarr("RLX", ParticleProperties::RLX);
        p_file.getarr("RLY", ParticleProperties::RLY);
        p_file.getarr("RLZ", ParticleProperties::RLZ);
        p_file.get("LOOP", ParticleProperties::loop_time);
        p_file.query("verbose", ParticleProperties::verbose);
        p_file.get("euler_velocity_index", ParticleProperties::euler_velocity_index);
        p_file.get("euler_force_index", ParticleProperties::euler_force_index);
        p_file.get("euler_fluid_rho", ParticleProperties::euler_fluid_rho);
        ParmParse level_parse("amr");
        level_parse.get("max_level", ParticleProperties::euler_finest_level);
        amrex::Print() << "[Particle] : Reading partilces cfg file : " << particle_inputfile << "\n"
                       << "             Particle's level : " << ParticleProperties::euler_finest_level << "\n";
    }else {
        amrex::Abort("[Particle] : can't read particles settings, pls check your config file \"particle.input\"");
    }
}

int Particles::ParticleFinestLevel()
{
    return ParticleProperties::euler_finest_level;
}
