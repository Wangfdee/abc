/**CFile****************************************************************

  FileName    [sbdCut.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [SAT-based optimization using internal don't-cares.]

  Synopsis    [Cut computation.]

  Author      [Alan Mishchenko]
  
  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - June 20, 2005.]

  Revision    [$Id: sbdCut.c,v 1.00 2005/06/20 00:00:00 alanmi Exp $]

***********************************************************************/

#include "sbdInt.h"

ABC_NAMESPACE_IMPL_START


////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

#define SBD_MAX_CUTSIZE   8
#define SBD_MAX_CUTNUM    64
#define SBD_MAX_TT_WORDS  ((SBD_MAX_CUTSIZE > 6) ? 1 << (SBD_MAX_CUTSIZE-6) : 1)

#define SBD_CUT_NO_LEAF   255

typedef struct Sbd_Cut_t_ Sbd_Cut_t; 
struct Sbd_Cut_t_
{
    word            Sign;                      // signature
    int             iFunc;                     // functionality
    unsigned        Cost      : 24;            // misc cut cost
    unsigned        nLeaves   :  8;            // the number of leaves
    int             pLeaves[SBD_MAX_CUTSIZE];  // leaves
};

typedef struct Sbd_Sto_t_ Sbd_Sto_t;
struct Sbd_Sto_t_
{
    int             nLutSize;
    int             nCutNum;
    int             fCutMin;
    int             fVerbose;
    Gia_Man_t *     pGia;                      // user's AIG manager (will be modified by adding nodes)
    Vec_Int_t *     vDelays;                   // delays for each node
    Vec_Wec_t *     vCuts;                     // cuts for each node
    Vec_Mem_t *     vTtMem;                    // truth tables
    Sbd_Cut_t       pCuts[3][SBD_MAX_CUTNUM];  // temporary cuts
    Sbd_Cut_t *     ppCuts[SBD_MAX_CUTNUM];    // temporary cut pointers
    abctime         clkStart;                  // starting time
    double          CutCount[4];               // cut counters
};

static inline word * Sbd_CutTruth( Sbd_Sto_t * p, Sbd_Cut_t * pCut ) { return Vec_MemReadEntry(p->vTtMem, Abc_Lit2Var(pCut->iFunc));           }

#define Sbd_ForEachCut( pList, pCut, i ) for ( i = 0, pCut = pList + 1; i < pList[0]; i++, pCut += pCut[0] + 2 )

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Sbd_Sto_t * Sbd_StoAlloc( Gia_Man_t * pGia, int nLutSize, int nCutNum, int fCutMin, int fVerbose )
{
    Sbd_Sto_t * p;
    assert( nLutSize > 1 && nLutSize <= SBD_MAX_CUTSIZE );
    assert( nCutNum > 1 && nCutNum <  SBD_MAX_CUTNUM );
    p = ABC_CALLOC( Sbd_Sto_t, 1 );
    p->clkStart  = Abc_Clock();
    p->nLutSize   = nLutSize;
    p->nCutNum   = nCutNum;
    p->fCutMin   = fCutMin;
    p->fVerbose  = fVerbose;
    p->pGia      = pGia;
    p->vDelays   = Vec_IntStart( Gia_ManObjNum(pGia) );
    p->vCuts     = Vec_WecStart( Gia_ManObjNum(pGia) );
    p->vTtMem    = fCutMin ? Vec_MemAllocForTT( nLutSize, 0 ) : NULL;
    return p;
}
void Sbd_StoFree( Sbd_Sto_t * p )
{
    Vec_IntFree( p->vDelays );
    Vec_WecFree( p->vCuts );
    if ( p->fCutMin )
        Vec_MemHashFree( p->vTtMem );
    if ( p->fCutMin )
        Vec_MemFree( p->vTtMem );
    ABC_FREE( p );
}

/**Function*************************************************************

  Synopsis    [Check correctness of cuts.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline word Sbd_CutGetSign( Sbd_Cut_t * pCut )
{
    word Sign = 0; int i; 
    for ( i = 0; i < (int)pCut->nLeaves; i++ )
        Sign |= ((word)1) << (pCut->pLeaves[i] & 0x3F);
    return Sign;
}
static inline int Sbd_CutCheck( Sbd_Cut_t * pBase, Sbd_Cut_t * pCut ) // check if pCut is contained in pBase
{
    int nSizeB = pBase->nLeaves;
    int nSizeC = pCut->nLeaves;
    int i, * pB = pBase->pLeaves;
    int k, * pC = pCut->pLeaves;
    for ( i = 0; i < nSizeC; i++ )
    {
        for ( k = 0; k < nSizeB; k++ )
            if ( pC[i] == pB[k] )
                break;
        if ( k == nSizeB )
            return 0;
    }
    return 1;
}
static inline int Sbd_CutSetCheckArray( Sbd_Cut_t ** ppCuts, int nCuts )
{
    Sbd_Cut_t * pCut0, * pCut1; 
    int i, k, m, n, Value;
    assert( nCuts > 0 );
    for ( i = 0; i < nCuts; i++ )
    {
        pCut0 = ppCuts[i];
        assert( pCut0->nLeaves <= SBD_MAX_CUTSIZE );
        assert( pCut0->Sign == Sbd_CutGetSign(pCut0) );
        // check duplicates
        for ( m = 0; m < (int)pCut0->nLeaves; m++ )
        for ( n = m + 1; n < (int)pCut0->nLeaves; n++ )
            assert( pCut0->pLeaves[m] < pCut0->pLeaves[n] );
        // check pairs
        for ( k = 0; k < nCuts; k++ )
        {
            pCut1 = ppCuts[k];
            if ( pCut0 == pCut1 )
                continue;
            // check containments
            Value = Sbd_CutCheck( pCut0, pCut1 );
            assert( Value == 0 );
        }
    }
    return 1;
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline int Sbd_CutMergeOrder( Sbd_Cut_t * pCut0, Sbd_Cut_t * pCut1, Sbd_Cut_t * pCut, int nLutSize )
{ 
    int nSize0   = pCut0->nLeaves;
    int nSize1   = pCut1->nLeaves;
    int i, * pC0 = pCut0->pLeaves;
    int k, * pC1 = pCut1->pLeaves;
    int c, * pC  = pCut->pLeaves;
    // the case of the largest cut sizes
    if ( nSize0 == nLutSize && nSize1 == nLutSize )
    {
        for ( i = 0; i < nSize0; i++ )
        {
            if ( pC0[i] != pC1[i] )  return 0;
            pC[i] = pC0[i];
        }
        pCut->nLeaves = nLutSize;
        pCut->iFunc = -1;
        pCut->Sign = pCut0->Sign | pCut1->Sign;
        return 1;
    }
    // compare two cuts with different numbers
    i = k = c = 0;
    if ( nSize0 == 0 ) goto FlushCut1;
    if ( nSize1 == 0 ) goto FlushCut0;
    while ( 1 )
    {
        if ( c == nLutSize ) return 0;
        if ( pC0[i] < pC1[k] )
        {
            pC[c++] = pC0[i++];
            if ( i >= nSize0 ) goto FlushCut1;
        }
        else if ( pC0[i] > pC1[k] )
        {
            pC[c++] = pC1[k++];
            if ( k >= nSize1 ) goto FlushCut0;
        }
        else
        {
            pC[c++] = pC0[i++]; k++;
            if ( i >= nSize0 ) goto FlushCut1;
            if ( k >= nSize1 ) goto FlushCut0;
        }
    }

FlushCut0:
    if ( c + nSize0 > nLutSize + i ) return 0;
    while ( i < nSize0 )
        pC[c++] = pC0[i++];
    pCut->nLeaves = c;
    pCut->iFunc = -1;
    pCut->Sign = pCut0->Sign | pCut1->Sign;
    return 1;

FlushCut1:
    if ( c + nSize1 > nLutSize + k ) return 0;
    while ( k < nSize1 )
        pC[c++] = pC1[k++];
    pCut->nLeaves = c;
    pCut->iFunc = -1;
    pCut->Sign = pCut0->Sign | pCut1->Sign;
    return 1;
}
static inline int Sbd_CutMergeOrder2( Sbd_Cut_t * pCut0, Sbd_Cut_t * pCut1, Sbd_Cut_t * pCut, int nLutSize )
{ 
    int x0, i0 = 0, nSize0 = pCut0->nLeaves, * pC0 = pCut0->pLeaves;
    int x1, i1 = 0, nSize1 = pCut1->nLeaves, * pC1 = pCut1->pLeaves;
    int xMin, c = 0, * pC  = pCut->pLeaves;
    while ( 1 )
    {
        x0 = (i0 == nSize0) ? ABC_INFINITY : pC0[i0];
        x1 = (i1 == nSize1) ? ABC_INFINITY : pC1[i1];
        xMin = Abc_MinInt(x0, x1);
        if ( xMin == ABC_INFINITY ) break;
        if ( c == nLutSize ) return 0;
        pC[c++] = xMin;
        if (x0 == xMin) i0++;
        if (x1 == xMin) i1++;
    }
    pCut->nLeaves = c;
    pCut->iFunc = -1;
    pCut->Sign = pCut0->Sign | pCut1->Sign;
    return 1;
}
static inline int Sbd_CutSetCutIsContainedOrder( Sbd_Cut_t * pBase, Sbd_Cut_t * pCut ) // check if pCut is contained in pBase
{
    int i, nSizeB = pBase->nLeaves;
    int k, nSizeC = pCut->nLeaves;
    if ( nSizeB == nSizeC )
    {
        for ( i = 0; i < nSizeB; i++ )
            if ( pBase->pLeaves[i] != pCut->pLeaves[i] )
                return 0;
        return 1;
    }
    assert( nSizeB > nSizeC ); 
    if ( nSizeC == 0 )
        return 1;
    for ( i = k = 0; i < nSizeB; i++ )
    {
        if ( pBase->pLeaves[i] > pCut->pLeaves[k] )
            return 0;
        if ( pBase->pLeaves[i] == pCut->pLeaves[k] )
        {
            if ( ++k == nSizeC )
                return 1;
        }
    }
    return 0;
}
static inline int Sbd_CutSetLastCutIsContained( Sbd_Cut_t ** pCuts, int nCuts )
{
    int i;
    for ( i = 0; i < nCuts; i++ )
        if ( pCuts[i]->nLeaves <= pCuts[nCuts]->nLeaves && (pCuts[i]->Sign & pCuts[nCuts]->Sign) == pCuts[i]->Sign && Sbd_CutSetCutIsContainedOrder(pCuts[nCuts], pCuts[i]) )
            return 1;
    return 0;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline int Sbd_CutCompare( Sbd_Cut_t * pCut0, Sbd_Cut_t * pCut1 )
{
    if ( pCut0->nLeaves < pCut1->nLeaves )  return -1;
    if ( pCut0->nLeaves > pCut1->nLeaves )  return  1;
    if ( pCut0->Cost    < pCut1->Cost    )  return -1;
    if ( pCut0->Cost    > pCut1->Cost    )  return  1;
    return 0;
}
static inline int Sbd_CutSetLastCutContains( Sbd_Cut_t ** pCuts, int nCuts )
{
    int i, k, fChanges = 0;
    for ( i = 0; i < nCuts; i++ )
        if ( pCuts[nCuts]->nLeaves < pCuts[i]->nLeaves && (pCuts[nCuts]->Sign & pCuts[i]->Sign) == pCuts[nCuts]->Sign && Sbd_CutSetCutIsContainedOrder(pCuts[i], pCuts[nCuts]) )
            pCuts[i]->nLeaves = SBD_CUT_NO_LEAF, fChanges = 1;
    if ( !fChanges )
        return nCuts;
    for ( i = k = 0; i <= nCuts; i++ )
    {
        if ( pCuts[i]->nLeaves == SBD_CUT_NO_LEAF )
            continue;
        if ( k < i )
            ABC_SWAP( Sbd_Cut_t *, pCuts[k], pCuts[i] );
        k++;
    }
    return k - 1;
}
static inline void Sbd_CutSetSortByCost( Sbd_Cut_t ** pCuts, int nCuts )
{
    int i;
    for ( i = nCuts; i > 0; i-- )
    {
        if ( Sbd_CutCompare(pCuts[i - 1], pCuts[i]) < 0 )//!= 1 )
            return;
        ABC_SWAP( Sbd_Cut_t *, pCuts[i - 1], pCuts[i] );
    }
}
static inline int Sbd_CutSetAddCut( Sbd_Cut_t ** pCuts, int nCuts, int nCutNum )
{
    if ( nCuts == 0 )
        return 1;
    nCuts = Sbd_CutSetLastCutContains(pCuts, nCuts);
    assert( nCuts >= 0 );
    Sbd_CutSetSortByCost( pCuts, nCuts );
    // add new cut if there is room
    return Abc_MinInt( nCuts + 1, nCutNum - 1 );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline int Sbd_CutComputeTruth6( Sbd_Sto_t * p, Sbd_Cut_t * pCut0, Sbd_Cut_t * pCut1, int fCompl0, int fCompl1, Sbd_Cut_t * pCutR, int fIsXor )
{
    int nOldSupp = pCutR->nLeaves, truthId, fCompl; word t;
    word t0 = *Sbd_CutTruth(p, pCut0);
    word t1 = *Sbd_CutTruth(p, pCut1);
    if ( Abc_LitIsCompl(pCut0->iFunc) ^ fCompl0 ) t0 = ~t0;
    if ( Abc_LitIsCompl(pCut1->iFunc) ^ fCompl1 ) t1 = ~t1;
    t0 = Abc_Tt6Expand( t0, pCut0->pLeaves, pCut0->nLeaves, pCutR->pLeaves, pCutR->nLeaves );
    t1 = Abc_Tt6Expand( t1, pCut1->pLeaves, pCut1->nLeaves, pCutR->pLeaves, pCutR->nLeaves );
    t =  fIsXor ? t0 ^ t1 : t0 & t1;
    if ( (fCompl = (int)(t & 1)) ) t = ~t;
    pCutR->nLeaves = Abc_Tt6MinBase( &t, pCutR->pLeaves, pCutR->nLeaves );
    assert( (int)(t & 1) == 0 );
    truthId        = Vec_MemHashInsert(p->vTtMem, &t);
    pCutR->iFunc   = Abc_Var2Lit( truthId, fCompl );
    assert( (int)pCutR->nLeaves <= nOldSupp );
    return (int)pCutR->nLeaves < nOldSupp;
}
static inline int Sbd_CutComputeTruth( Sbd_Sto_t * p, Sbd_Cut_t * pCut0, Sbd_Cut_t * pCut1, int fCompl0, int fCompl1, Sbd_Cut_t * pCutR, int fIsXor )
{
    if ( p->nLutSize <= 6 )
        return Sbd_CutComputeTruth6( p, pCut0, pCut1, fCompl0, fCompl1, pCutR, fIsXor );
    {
    word uTruth[SBD_MAX_TT_WORDS], uTruth0[SBD_MAX_TT_WORDS], uTruth1[SBD_MAX_TT_WORDS];
    int nOldSupp   = pCutR->nLeaves, truthId;
    int nLutSize   = p->nLutSize, fCompl;
    int nWords     = Abc_Truth6WordNum(nLutSize);
    word * pTruth0 = Sbd_CutTruth(p, pCut0);
    word * pTruth1 = Sbd_CutTruth(p, pCut1);
    Abc_TtCopy( uTruth0, pTruth0, nWords, Abc_LitIsCompl(pCut0->iFunc) ^ fCompl0 );
    Abc_TtCopy( uTruth1, pTruth1, nWords, Abc_LitIsCompl(pCut1->iFunc) ^ fCompl1 );
    Abc_TtExpand( uTruth0, nLutSize, pCut0->pLeaves, pCut0->nLeaves, pCutR->pLeaves, pCutR->nLeaves );
    Abc_TtExpand( uTruth1, nLutSize, pCut1->pLeaves, pCut1->nLeaves, pCutR->pLeaves, pCutR->nLeaves );
    if ( fIsXor )
        Abc_TtXor( uTruth, uTruth0, uTruth1, nWords, (fCompl = (int)((uTruth0[0] ^ uTruth1[0]) & 1)) );
    else
        Abc_TtAnd( uTruth, uTruth0, uTruth1, nWords, (fCompl = (int)((uTruth0[0] & uTruth1[0]) & 1)) );
    pCutR->nLeaves = Abc_TtMinBase( uTruth, pCutR->pLeaves, pCutR->nLeaves, nLutSize );
    assert( (uTruth[0] & 1) == 0 );
//Kit_DsdPrintFromTruth( uTruth, pCutR->nLeaves ), printf("\n" ), printf("\n" );
    truthId        = Vec_MemHashInsert(p->vTtMem, uTruth);
    pCutR->iFunc   = Abc_Var2Lit( truthId, fCompl );
    assert( (int)pCutR->nLeaves <= nOldSupp );
    return (int)pCutR->nLeaves < nOldSupp;
    }
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static inline int Sbd_CutCountBits( word i )
{
    i = i - ((i >> 1) & 0x5555555555555555);
    i = (i & 0x3333333333333333) + ((i >> 2) & 0x3333333333333333);
    i = ((i + (i >> 4)) & 0x0F0F0F0F0F0F0F0F);
    return (i*(0x0101010101010101))>>56;
}
static inline void Sbd_CutPrint( Sbd_Sto_t * p, Sbd_Cut_t * pCut )
{
    int i, nDigits = Abc_Base10Log(Gia_ManObjNum(p->pGia)); 
    printf( "%d  {", pCut->nLeaves );
    for ( i = 0; i < (int)pCut->nLeaves; i++ )
        printf( " %*d", nDigits, pCut->pLeaves[i] );
    for ( ; i < (int)p->nLutSize; i++ )
        printf( " %*s", nDigits, " " );
    printf( "  }  Cost = %4d\n", pCut->Cost );
}
static inline int Sbd_CutCost( Sbd_Sto_t * p, Sbd_Cut_t * pCut )
{
    int i, Cost = 0;
    for ( i = 0; i < (int)pCut->nLeaves; i++ )
        Cost += Vec_IntEntry( p->vDelays, pCut->pLeaves[i] );
    return Cost;
}
static inline void Sbd_CutAddUnitCut( Sbd_Sto_t * p, int iObj )
{
    Vec_Int_t * vThis = Vec_WecEntry( p->vCuts, iObj );
    if ( Vec_IntSize(vThis) == 0 )
        Vec_IntPush( vThis, 1 );
    else 
        Vec_IntAddToEntry( vThis, 0, 1 );
    Vec_IntPush( vThis, 1 );
    Vec_IntPush( vThis, iObj );
    Vec_IntPush( vThis, 2 );
}
static inline int Sbd_StoPrepareSet( Sbd_Sto_t * p, int iObj, int Index )
{
    Vec_Int_t * vThis = Vec_WecEntry( p->vCuts, iObj );
    int i, v, * pCut, * pList = Vec_IntArray( vThis );
    Sbd_ForEachCut( pList, pCut, i )
    {
        Sbd_Cut_t * pCutTemp = &p->pCuts[Index][i];
        pCutTemp->nLeaves = pCut[0];
        for ( v = 1; v <= pCut[0]; v++ )
            pCutTemp->pLeaves[v-1] = pCut[v];
        pCutTemp->iFunc = pCut[pCut[0]+1];
        pCutTemp->Sign = Sbd_CutGetSign( pCutTemp );
        pCutTemp->Cost = Sbd_CutCost( p, pCutTemp );
    }
    return pList[0];
}
static inline void Sbd_StoInitResult( Sbd_Sto_t * p )
{
    int i; 
    for ( i = 0; i < SBD_MAX_CUTNUM; i++ )
        p->ppCuts[i] = &p->pCuts[2][i];
}
static inline void Sbd_StoStoreResult( Sbd_Sto_t * p, int iObj, Sbd_Cut_t ** pCuts, int nCuts )
{
    int i, v;
    Vec_Int_t * vList = Vec_WecEntry( p->vCuts, iObj );
    Vec_IntPush( vList, nCuts );
    for ( i = 0; i < nCuts; i++ )
    {
        Vec_IntPush( vList, pCuts[i]->nLeaves );
        for ( v = 0; v < (int)pCuts[i]->nLeaves; v++ )
            Vec_IntPush( vList, pCuts[i]->pLeaves[v] );
        Vec_IntPush( vList, pCuts[i]->iFunc );
    }
}
static inline void Sbd_StoComputeDelay( Sbd_Sto_t * p, int iObj, Sbd_Cut_t ** pCuts, int nCuts )
{
    int i, v, Delay, DelayMin = ABC_INFINITY;
    assert( nCuts > 0 );
    for ( i = 0; i < nCuts; i++ )
    {
        if ( (int)pCuts[i]->nLeaves > p->nLutSize )
            continue;
        Delay = 0;
        for ( v = 0; v < (int)pCuts[i]->nLeaves; v++ )
            Delay = Abc_MaxInt( Delay, Vec_IntEntry(p->vDelays, pCuts[i]->pLeaves[v]) );
        DelayMin = Abc_MinInt( DelayMin, Delay );
    }
    assert( DelayMin < ABC_INFINITY );
    Vec_IntWriteEntry( p->vDelays, iObj, (nCuts > 1 || pCuts[0]->nLeaves > 1) ? DelayMin + 1 : DelayMin );
}
void Sbd_StoMergeCuts( Sbd_Sto_t * p, int iObj )
{
    Gia_Obj_t * pObj = Gia_ManObj(p->pGia, iObj);
    int fIsXor       = Gia_ObjIsXor(pObj);
    int nLutSize     = p->nLutSize;
    int nCutNum      = p->nCutNum;
    int fComp0       = Gia_ObjFaninC0(pObj);
    int fComp1       = Gia_ObjFaninC1(pObj);
    int nCuts0       = Sbd_StoPrepareSet( p, Gia_ObjFaninId0(pObj, iObj), 0 );
    int nCuts1       = Sbd_StoPrepareSet( p, Gia_ObjFaninId1(pObj, iObj), 1 );
    int i, k, nCutsR = 0;
    Sbd_Cut_t * pCut0, * pCut1, ** pCutsR = p->ppCuts;
    assert( !Gia_ObjIsBuf(pObj) );
    assert( !Gia_ObjIsMux(p->pGia, pObj) );
    Sbd_StoInitResult( p );
    p->CutCount[0] += nCuts0 * nCuts1;
    for ( i = 0, pCut0 = p->pCuts[0]; i < nCuts0; i++, pCut0++ )
    for ( k = 0, pCut1 = p->pCuts[1]; k < nCuts1; k++, pCut1++ )
    {
        if ( (int)(pCut0->nLeaves + pCut1->nLeaves) > nLutSize && Sbd_CutCountBits(pCut0->Sign | pCut1->Sign) > nLutSize )
            continue;
        p->CutCount[1]++; 
        if ( !Sbd_CutMergeOrder(pCut0, pCut1, pCutsR[nCutsR], nLutSize) )
            continue;
        if ( Sbd_CutSetLastCutIsContained(pCutsR, nCutsR) )
            continue;
        p->CutCount[2]++;
        if ( p->fCutMin && Sbd_CutComputeTruth(p, pCut0, pCut1, fComp0, fComp1, pCutsR[nCutsR], fIsXor) )
            pCutsR[nCutsR]->Sign = Sbd_CutGetSign(pCutsR[nCutsR]);
        pCutsR[nCutsR]->Cost = Sbd_CutCost( p, pCutsR[nCutsR] );
        nCutsR = Sbd_CutSetAddCut( pCutsR, nCutsR, nCutNum );
    }
    Sbd_StoComputeDelay( p, iObj, pCutsR, nCutsR );
    p->CutCount[3] += nCutsR;
    // debug printout
    if ( 0 )
    {
        printf( "*** Obj = %d  Delay = %d\n", iObj, Vec_IntEntry(p->vDelays, iObj) );
        for ( i = 0; i < nCutsR; i++ )
            Sbd_CutPrint( p, pCutsR[i] );
        printf( "\n" );
    }
    // verify
    assert( nCutsR > 0 && nCutsR < nCutNum );
    assert( Sbd_CutSetCheckArray(pCutsR, nCutsR) );
    // store the cutset
    Sbd_StoStoreResult( p, iObj, pCutsR, nCutsR );
    if ( nCutsR > 1 || pCutsR[0]->nLeaves > 1 )
        Sbd_CutAddUnitCut( p, iObj );
}
int Sbd_StoMergeCutsPlus( Sbd_Sto_t * p, int iObj )
{
    assert( iObj == Vec_IntSize(p->vDelays) );
    assert( iObj == Vec_WecSize(p->vCuts) );
    Vec_IntPush( p->vDelays, 0 );
    Vec_WecPushLevel( p->vCuts );
    Sbd_StoMergeCuts( p, iObj );
    return Vec_IntEntry( p->vDelays, iObj );
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Sbd_StoComputeCuts( Sbd_Sto_t * p )
{
    Gia_Obj_t * pObj;   
    int i, iObj;
    Gia_ManForEachCiId( p->pGia, iObj, i )
        Sbd_CutAddUnitCut( p, iObj );
    Gia_ManForEachAnd( p->pGia, pObj, iObj )
        Sbd_StoMergeCuts( p, iObj );
    if ( !p->fVerbose )
        return;
    printf( "CutPair = %.0f  ",         p->CutCount[0] );
    printf( "Merge = %.0f (%.2f %%)  ", p->CutCount[1], 100.0*p->CutCount[1]/p->CutCount[0] );
    printf( "Eval = %.0f (%.2f %%)  ",  p->CutCount[2], 100.0*p->CutCount[2]/p->CutCount[0] );
    printf( "Cut = %.0f (%.2f %%)  ",   p->CutCount[3], 100.0*p->CutCount[3]/p->CutCount[0] );
    printf( "\n" );
}
void Sbd_StoComputeCutsTest( Gia_Man_t * pGia )
{
    Sbd_Sto_t * p = Sbd_StoAlloc( pGia, 8, 32, 1, 1 );
    Sbd_StoComputeCuts( p );
    Sbd_StoFree( p );
}


////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


ABC_NAMESPACE_IMPL_END

