/*
  Stockfish, a UCI chess playing engine derived from Glaurung 2.1
  Copyright (C) 2004-2008 Tord Romstad (Glaurung author)
  Copyright (C) 2008-2014 Marco Costalba, Joona Kiiski, Tord Romstad

  Stockfish is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  Stockfish is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <sstream>

#include "bitcount.h"
#include "evaluate.h"
#include "material.h"
#include "pawns.h"
#include "thread.h"
#include "uci.h"

namespace {

  // Struct EvalInfo contains various information computed and collected
  // by the evaluation functions.
  struct EvalInfo {

    // Pointers to material and pawn hash table entries
    Material::Entry* mi;
    Pawns::Entry* pi;

    // attackedBy[color][piece type] is a bitboard representing all squares
    // attacked by a given color and piece type, attackedBy[color][ALL_PIECES]
    // contains all squares attacked by the given color.
    Bitboard attackedBy[COLOR_NB][PIECE_TYPE_NB];

    // kingRing[color] is the zone around the king which is considered
    // by the king safety evaluation. This consists of the squares directly
    // adjacent to the king, and the three (or two, for a king on an edge file)
    // squares two ranks in front of the king. For instance, if black's king
    // is on g8, kingRing[BLACK] is a bitboard containing the squares f8, h8,
    // f7, g7, h7, f6, g6 and h6.
    Bitboard kingRing[COLOR_NB];

    // kingAttackersCount[color] is the number of pieces of the given color
    // which attack a square in the kingRing of the enemy king.
    int kingAttackersCount[COLOR_NB];

    // kingAttackersWeight[color] is the sum of the "weight" of the pieces of the
    // given color which attack a square in the kingRing of the enemy king. The
    // weights of the individual piece types are given by the variables
    // QueenAttackWeight, RookAttackWeight, BishopAttackWeight and
    // KnightAttackWeight in evaluate.cpp
    int kingAttackersWeight[COLOR_NB];

    // kingAdjacentZoneAttacksCount[color] is the number of attacks to squares
    // directly adjacent to the king of the given color. Pieces which attack
    // more than one square are counted multiple times. For instance, if black's
    // king is on g8 and there's a white knight on g5, this knight adds
    // 2 to kingAdjacentZoneAttacksCount[BLACK].
    int kingAdjacentZoneAttacksCount[COLOR_NB];

    Bitboard pinnedPieces[COLOR_NB];
  };

  namespace Tracing {

    enum Terms { // First 8 entries are for PieceType
      MATERIAL = 8, IMBALANCE, MOBILITY, THREAT, PASSED, SPACE, TOTAL, TERMS_NB
    };

    Score scores[COLOR_NB][TERMS_NB];
    EvalInfo ei;
    ScaleFactor sf;

    double to_cp(Value v);
    void write(int idx, Color c, Score s);
    void write(int idx, Score w, Score b = SCORE_ZERO);
    void print(std::stringstream& ss, const char* name, int idx);
    std::string do_trace(const Position& pos);
  }

  // Evaluation weights, indexed by evaluation term
  enum { Mobility, PawnStructure, PassedPawns, Space, KingSafety };
  const struct Weight { int mg, eg; } Weights[] = {
    {289, 344}, {233, 201}, {221, 273}, {46, 0}, {318, 0}
  };

  typedef Value V;
  #define S(mg, eg) make_score(mg, eg)

  // MobilityBonus[PieceType][attacked] contains bonuses for middle and end
  // game, indexed by piece type and number of attacked squares not occupied by
  // friendly pieces.
  const Score MobilityBonus[][32] = {
    {}, {},
    { S(-65,-50), S(-42,-30), S(-9,-10), S( 3,  0), S(15, 10), S(27, 20), // Knights
      S( 37, 28), S( 42, 31), S(44, 33) },
    { S(-52,-47), S(-28,-23), S( 6,  1), S(20, 15), S(34, 29), S(48, 43), // Bishops
      S( 60, 55), S( 68, 63), S(74, 68), S(77, 72), S(80, 75), S(82, 77),
      S( 84, 79), S( 86, 81) },
    { S(-47,-53), S(-31,-26), S(-5,  0), S( 1, 16), S( 7, 32), S(13, 48), // Rooks
      S( 18, 64), S( 22, 80), S(26, 96), S(29,109), S(31,115), S(33,119),
      S( 35,122), S( 36,123), S(37,124) },
    { S(-42,-40), S(-28,-23), S(-5, -7), S( 0,  0), S( 6, 10), S(11, 19), // Queens
      S( 13, 29), S( 18, 38), S(20, 40), S(21, 41), S(22, 41), S(22, 41),
      S( 22, 41), S( 23, 41), S(24, 41), S(25, 41), S(25, 41), S(25, 41),
      S( 25, 41), S( 25, 41), S(25, 41), S(25, 41), S(25, 41), S(25, 41),
      S( 25, 41), S( 25, 41), S(25, 41), S(25, 41) }
  };

  // Outpost[PieceType][Square] contains bonuses for knights and bishops outposts,
  // indexed by piece type and square (from white's point of view).
  const Value Outpost[][SQUARE_NB] = {
  {// A     B     C     D     E     F     G     H
    V(0), V(0), V(0), V(0), V(0), V(0), V(0), V(0), // Knights
    V(0), V(0), V(0), V(0), V(0), V(0), V(0), V(0),
    V(0), V(0), V(4), V(8), V(8), V(4), V(0), V(0),
    V(0), V(4),V(17),V(26),V(26),V(17), V(4), V(0),
    V(0), V(8),V(26),V(35),V(35),V(26), V(8), V(0),
    V(0), V(4),V(17),V(17),V(17),V(17), V(4), V(0) },
  {
    V(0), V(0), V(0), V(0), V(0), V(0), V(0), V(0), // Bishops
    V(0), V(0), V(0), V(0), V(0), V(0), V(0), V(0),
    V(0), V(0), V(5), V(5), V(5), V(5), V(0), V(0),
    V(0), V(5),V(10),V(10),V(10),V(10), V(5), V(0),
    V(0),V(10),V(21),V(21),V(21),V(21),V(10), V(0),
    V(0), V(5), V(8), V(8), V(8), V(8), V(5), V(0) }
  };

  // Threat[attacking][attacked] contains bonuses according to which piece
  // type attacks which one.
  const Score Threat[][PIECE_TYPE_NB] = {
    { S(0, 0), S(0, 38), S(32, 45), S(32, 45), S(41,100), S(35,104) }, // Minor
    { S(0, 0), S(7, 28), S(20, 49), S(20, 49), S(8 , 42), S(23, 44) }  // Major
  };

  // ThreatenedByPawn[PieceType] contains a penalty according to which piece
  // type is attacked by an enemy pawn.
  const Score ThreatenedByPawn[] = {
    S(0, 0), S(0, 0), S(80, 119), S(80, 119), S(117, 199), S(127, 218)
  };

  // Assorted bonuses and penalties used by evaluation
  const Score KingOnOne        = S(2 , 58);
  const Score KingOnMany       = S(6 ,125);
  const Score RookOnPawn       = S(10, 28);
  const Score RookOpenFile     = S(43, 21);
  const Score RookSemiOpenFile = S(19, 10);
  const Score BishopPawns      = S( 8, 12);
  const Score MinorBehindPawn  = S(16,  0);
  const Score TrappedRook      = S(92,  0);
  const Score Unstoppable      = S( 0, 20);
  const Score Hanging          = S(23, 20);

  // Penalty for a bishop on a1/h1 (a8/h8 for black) which is trapped by
  // a friendly pawn on b2/g2 (b7/g7 for black). This can obviously only
  // happen in Chess960 games.
  const Score TrappedBishopA1H1 = S(50, 50);

  #undef S

  // SpaceMask[Color] contains the area of the board which is considered
  // by the space evaluation. In the middlegame, each side is given a bonus
  // based on how many squares inside this area are safe and available for
  // friendly minor pieces.
  const Bitboard SpaceMask[] = {
    (FileCBB | FileDBB | FileEBB | FileFBB) & (Rank2BB | Rank3BB | Rank4BB),
    (FileCBB | FileDBB | FileEBB | FileFBB) & (Rank7BB | Rank6BB | Rank5BB)
  };

  // King danger constants and variables. The king danger scores are taken
  // from KingDanger[]. Various little "meta-bonuses" measuring the strength
  // of the enemy attack are added up into an integer, which is used as an
  // index to KingDanger[].
  //
  // KingAttackWeights[PieceType] contains king attack weights by piece type
  const int KingAttackWeights[] = { 0, 0, 2, 2, 3, 5 };

  // Bonuses for enemy's safe checks
  const int QueenContactCheck = 24;
  const int RookContactCheck  = 16;
  const int QueenCheck        = 12;
  const int RookCheck         = 8;
  const int BishopCheck       = 2;
  const int KnightCheck       = 3;

  // KingDanger[attackUnits] contains the actual king danger weighted
  // scores, indexed by a calculated integer number.
  Score KingDanger[128];

  const int ScalePawnSpan[2] = { 38, 56 };

  // apply_weight() weighs score 'v' by weight 'w' trying to prevent overflow
  Score apply_weight(Score v, const Weight& w) {
    return make_score(mg_value(v) * w.mg / 256, eg_value(v) * w.eg / 256);
  }


  // init_eval_info() initializes king bitboards for given color adding
  // pawn attacks. To be done at the beginning of the evaluation.

  template<Color Us>
  void init_eval_info(const Position& pos, EvalInfo& ei) {

    const Color  Them = (Us == WHITE ? BLACK : WHITE);
    const Square Down = (Us == WHITE ? DELTA_S : DELTA_N);

    ei.pinnedPieces[Us] = pos.pinned_pieces(Us);

    Bitboard b = ei.attackedBy[Them][KING] = pos.attacks_from<KING>(pos.king_square(Them));
    ei.attackedBy[Us][ALL_PIECES] = ei.attackedBy[Us][PAWN] = ei.pi->pawn_attacks(Us);

    // Init king safety tables only if we are going to use them
    if (pos.non_pawn_material(Us) > QueenValueMg + PawnValueMg)
    {
        ei.kingRing[Them] = b | shift_bb<Down>(b);
        b &= ei.attackedBy[Us][PAWN];
        ei.kingAttackersCount[Us] = b ? popcount<Max15>(b) : 0;
        ei.kingAdjacentZoneAttacksCount[Us] = ei.kingAttackersWeight[Us] = 0;
    }
    else
        ei.kingRing[Them] = ei.kingAttackersCount[Us] = 0;
  }


  // evaluate_outpost() evaluates bishop and knight outpost squares

  template<PieceType Pt, Color Us>
  Score evaluate_outpost(const Position& pos, const EvalInfo& ei, Square s) {

    const Color Them = (Us == WHITE ? BLACK : WHITE);

    assert (Pt == BISHOP || Pt == KNIGHT);

    // Initial bonus based on square
    Value bonus = Outpost[Pt == BISHOP][relative_square(Us, s)];

    // Increase bonus if supported by pawn, especially if the opponent has
    // no minor piece which can trade with the outpost piece.
    if (bonus && (ei.attackedBy[Us][PAWN] & s))
    {
        if (   !pos.pieces(Them, KNIGHT)
            && !(squares_of_color(s) & pos.pieces(Them, BISHOP)))
            bonus += bonus + bonus / 2;
        else
            bonus += bonus / 2;
    }

    return make_score(bonus * 2, bonus / 2);
  }


  // evaluate_pieces() assigns bonuses and penalties to the pieces of a given color

  template<PieceType Pt, Color Us, bool Trace>
  Score evaluate_pieces(const Position& pos, EvalInfo& ei, Score* mobility, Bitboard* mobilityArea) {

    Bitboard b;
    Square s;
    Score score = SCORE_ZERO;

    const PieceType NextPt = (Us == WHITE ? Pt : PieceType(Pt + 1));
    const Color Them = (Us == WHITE ? BLACK : WHITE);
    const Square* pl = pos.list<Pt>(Us);

    ei.attackedBy[Us][Pt] = 0;

    while ((s = *pl++) != SQ_NONE)
    {
        // Find attacked squares, including x-ray attacks for bishops and rooks
        b = Pt == BISHOP ? attacks_bb<BISHOP>(s, pos.pieces() ^ pos.pieces(Us, QUEEN))
          : Pt ==   ROOK ? attacks_bb<  ROOK>(s, pos.pieces() ^ pos.pieces(Us, ROOK, QUEEN))
                         : pos.attacks_from<Pt>(s);

        if (ei.pinnedPieces[Us] & s)
            b &= LineBB[pos.king_square(Us)][s];

        ei.attackedBy[Us][ALL_PIECES] |= ei.attackedBy[Us][Pt] |= b;

        if (b & ei.kingRing[Them])
        {
            ei.kingAttackersCount[Us]++;
            ei.kingAttackersWeight[Us] += KingAttackWeights[Pt];
            Bitboard bb = b & ei.attackedBy[Them][KING];
            if (bb)
                ei.kingAdjacentZoneAttacksCount[Us] += popcount<Max15>(bb);
        }

        if (Pt == QUEEN)
            b &= ~(  ei.attackedBy[Them][KNIGHT]
                   | ei.attackedBy[Them][BISHOP]
                   | ei.attackedBy[Them][ROOK]);

        int mob = Pt != QUEEN ? popcount<Max15>(b & mobilityArea[Us])
                              : popcount<Full >(b & mobilityArea[Us]);

        mobility[Us] += MobilityBonus[Pt][mob];

        // Decrease score if we are attacked by an enemy pawn. The remaining part
        // of threat evaluation must be done later when we have full attack info.
        if (ei.attackedBy[Them][PAWN] & s)
            score -= ThreatenedByPawn[Pt];

        if (Pt == BISHOP || Pt == KNIGHT)
        {
            // Penalty for bishop with same colored pawns
            if (Pt == BISHOP)
                score -= BishopPawns * ei.pi->pawns_on_same_color_squares(Us, s);

            // Bishop and knight outpost square
            if (!(pos.pieces(Them, PAWN) & pawn_attack_span(Us, s)))
                score += evaluate_outpost<Pt, Us>(pos, ei, s);

            // Bishop or knight behind a pawn
            if (    relative_rank(Us, s) < RANK_5
                && (pos.pieces(PAWN) & (s + pawn_push(Us))))
                score += MinorBehindPawn;
        }

        if (Pt == ROOK)
        {
            // Rook piece attacking enemy pawns on the same rank/file
            if (relative_rank(Us, s) >= RANK_5)
            {
                Bitboard pawns = pos.pieces(Them, PAWN) & PseudoAttacks[ROOK][s];
                if (pawns)
                    score += popcount<Max15>(pawns) * RookOnPawn;
            }

            // Give a bonus for a rook on a open or semi-open file
            if (ei.pi->semiopen_file(Us, file_of(s)))
                score += ei.pi->semiopen_file(Them, file_of(s)) ? RookOpenFile : RookSemiOpenFile;

            if (mob > 3 || ei.pi->semiopen_file(Us, file_of(s)))
                continue;

            Square ksq = pos.king_square(Us);

            // Penalize rooks which are trapped by a king. Penalize more if the
            // king has lost its castling capability.
            if (   ((file_of(ksq) < FILE_E) == (file_of(s) < file_of(ksq)))
                && (rank_of(ksq) == rank_of(s) || relative_rank(Us, ksq) == RANK_1)
                && !ei.pi->semiopen_side(Us, file_of(ksq), file_of(s) < file_of(ksq)))
                score -= (TrappedRook - make_score(mob * 22, 0)) * (1 + !pos.can_castle(Us));
        }

        // An important Chess960 pattern: A cornered bishop blocked by a friendly
        // pawn diagonally in front of it is a very serious problem, especially
        // when that pawn is also blocked.
        if (   Pt == BISHOP
            && pos.is_chess960()
            && (s == relative_square(Us, SQ_A1) || s == relative_square(Us, SQ_H1)))
        {
            Square d = pawn_push(Us) + (file_of(s) == FILE_A ? DELTA_E : DELTA_W);
            if (pos.piece_on(s + d) == make_piece(Us, PAWN))
                score -= !pos.empty(s + d + pawn_push(Us))                ? TrappedBishopA1H1 * 4
                        : pos.piece_on(s + d + d) == make_piece(Us, PAWN) ? TrappedBishopA1H1 * 2
                                                                          : TrappedBishopA1H1;
        }
    }

    if (Trace)
        Tracing::write(Pt, Us, score);

    return score - evaluate_pieces<NextPt, Them, Trace>(pos, ei, mobility, mobilityArea);
  }

  template<>
  Score evaluate_pieces<KING, WHITE, false>(const Position&, EvalInfo&, Score*, Bitboard*) { return SCORE_ZERO; }
  template<>
  Score evaluate_pieces<KING, WHITE,  true>(const Position&, EvalInfo&, Score*, Bitboard*) { return SCORE_ZERO; }


  // evaluate_king() assigns bonuses and penalties to a king of a given color

  template<Color Us, bool Trace>
  Score evaluate_king(const Position& pos, const EvalInfo& ei) {

    const Color Them = (Us == WHITE ? BLACK : WHITE);

    Bitboard undefended, b, b1, b2, safe;
    int attackUnits;
    const Square ksq = pos.king_square(Us);

    // King shelter and enemy pawns storm
    Score score = ei.pi->king_safety<Us>(pos, ksq);

    // Main king safety evaluation
    if (ei.kingAttackersCount[Them])
    {
        // Find the attacked squares around the king which have no defenders
        // apart from the king itself
        undefended =  ei.attackedBy[Them][ALL_PIECES]
                    & ei.attackedBy[Us][KING]
                    & ~(  ei.attackedBy[Us][PAWN]   | ei.attackedBy[Us][KNIGHT]
                        | ei.attackedBy[Us][BISHOP] | ei.attackedBy[Us][ROOK]
                        | ei.attackedBy[Us][QUEEN]);

        // Initialize the 'attackUnits' variable, which is used later on as an
        // index to the KingDanger[] array. The initial value is based on the
        // number and types of the enemy's attacking pieces, the number of
        // attacked and undefended squares around our king and the quality of
        // the pawn shelter (current 'score' value).
        attackUnits =  std::min(20, (ei.kingAttackersCount[Them] * ei.kingAttackersWeight[Them]) / 2)
                     + 3 * (ei.kingAdjacentZoneAttacksCount[Them] + popcount<Max15>(undefended))
                     + 2 * (ei.pinnedPieces[Us] != 0)
                     - mg_value(score) / 32
                     - !pos.count<QUEEN>(Them) * 15;

        // Analyse the enemy's safe queen contact checks. Firstly, find the
        // undefended squares around the king that are attacked by the enemy's
        // queen...
        b = undefended & ei.attackedBy[Them][QUEEN] & ~pos.pieces(Them);
        if (b)
        {
            // ...and then remove squares not supported by another enemy piece
            b &= (  ei.attackedBy[Them][PAWN]   | ei.attackedBy[Them][KNIGHT]
                  | ei.attackedBy[Them][BISHOP] | ei.attackedBy[Them][ROOK]);

            if (b)
                attackUnits +=  QueenContactCheck * popcount<Max15>(b);
        }

        // Analyse the enemy's safe rook contact checks. Firstly, find the
        // undefended squares around the king that are attacked by the enemy's
        // rooks...
        b = undefended & ei.attackedBy[Them][ROOK] & ~pos.pieces(Them);

        // Consider only squares where the enemy's rook gives check
        b &= PseudoAttacks[ROOK][ksq];

        if (b)
        {
            // ...and then remove squares not supported by another enemy piece
            b &= (  ei.attackedBy[Them][PAWN]   | ei.attackedBy[Them][KNIGHT]
                  | ei.attackedBy[Them][BISHOP] | ei.attackedBy[Them][QUEEN]);

            if (b)
                attackUnits +=  RookContactCheck * popcount<Max15>(b);
        }

        // Analyse the enemy's safe distance checks for sliders and knights
        safe = ~(pos.pieces(Them) | ei.attackedBy[Us][ALL_PIECES]);

        b1 = pos.attacks_from<ROOK>(ksq) & safe;
        b2 = pos.attacks_from<BISHOP>(ksq) & safe;

        // Enemy queen safe checks
        b = (b1 | b2) & ei.attackedBy[Them][QUEEN];
        if (b)
            attackUnits += QueenCheck * popcount<Max15>(b);

        // Enemy rooks safe checks
        b = b1 & ei.attackedBy[Them][ROOK];
        if (b)
            attackUnits += RookCheck * popcount<Max15>(b);

        // Enemy bishops safe checks
        b = b2 & ei.attackedBy[Them][BISHOP];
        if (b)
            attackUnits += BishopCheck * popcount<Max15>(b);

        // Enemy knights safe checks
        b = pos.attacks_from<KNIGHT>(ksq) & ei.attackedBy[Them][KNIGHT] & safe;
        if (b)
            attackUnits += KnightCheck * popcount<Max15>(b);

        // To index KingDanger[] attackUnits must be in [0, 99] range
        attackUnits = std::min(99, std::max(0, attackUnits));

        // Finally, extract the king danger score from the KingDanger[]
        // array and subtract the score from evaluation.
        score -= KingDanger[attackUnits];
    }

    if (Trace)
        Tracing::write(KING, Us, score);

    return score;
  }


  // max_piece_type() is a helper function used by evaluate_threats() to get
  // the value of the biggest PieceType of color C in 'target' bitboard.

  template<Color C>
  inline PieceType max_piece_type(const Position& pos, const Bitboard target) {

    assert(target & (pos.pieces(C) ^ pos.pieces(C, KING)));

    for (PieceType pt = QUEEN; pt > PAWN; --pt)
        if (target & pos.pieces(C, pt))
            return pt;

    return PAWN;
  }


  // evaluate_threats() assigns bonuses according to the type of attacking piece
  // and the type of attacked one.

  template<Color Us, bool Trace>
  Score evaluate_threats(const Position& pos, const EvalInfo& ei) {

    const Color Them = (Us == WHITE ? BLACK : WHITE);

    enum { Minor, Major };

    Bitboard b, weakEnemies, protectedEnemies;
    Score score = SCORE_ZERO;

    // Enemies defended by a pawn and under our attack by a minor piece
    protectedEnemies =  (pos.pieces(Them) ^ pos.pieces(Them, PAWN))
                      &  ei.attackedBy[Them][PAWN]
                      & (ei.attackedBy[Us][KNIGHT] | ei.attackedBy[Us][BISHOP]);

    if (protectedEnemies)
        score += Threat[Minor][max_piece_type<Them>(pos, protectedEnemies)];

    // Enemies not defended by a pawn and under our attack
    weakEnemies =   pos.pieces(Them)
                 & ~ei.attackedBy[Them][PAWN]
                 &  ei.attackedBy[Us][ALL_PIECES];

    // Add a bonus according if the attacking pieces are minor or major
    if (weakEnemies)
    {
        b = weakEnemies & (ei.attackedBy[Us][KNIGHT] | ei.attackedBy[Us][BISHOP]);
        if (b)
            score += Threat[Minor][max_piece_type<Them>(pos, b)];

        b = weakEnemies & (ei.attackedBy[Us][ROOK] | ei.attackedBy[Us][QUEEN]);
        if (b)
            score += Threat[Major][max_piece_type<Them>(pos, b)];

        b = weakEnemies & ~ei.attackedBy[Them][ALL_PIECES];
        if (b)
            score += more_than_one(b) ? Hanging * popcount<Max15>(b) : Hanging;

        b = weakEnemies & ei.attackedBy[Us][KING];
        if (b)
            score += more_than_one(b) ? KingOnMany : KingOnOne;
    }

    if (Trace)
        Tracing::write(Tracing::THREAT, Us, score);

    return score;
  }


  // evaluate_passed_pawns() evaluates the passed pawns of the given color

  template<Color Us, bool Trace>
  Score evaluate_passed_pawns(const Position& pos, const EvalInfo& ei) {

    const Color Them = (Us == WHITE ? BLACK : WHITE);

    Bitboard b, squaresToQueen, defendedSquares, unsafeSquares;
    Score score = SCORE_ZERO;

    b = ei.pi->passed_pawns(Us);

    while (b)
    {
        Square s = pop_lsb(&b);

        assert(pos.pawn_passed(Us, s));

        int r = relative_rank(Us, s) - RANK_2;
        int rr = r * (r - 1);

        // Base bonus based on rank
        Value mbonus = Value(17 * rr), ebonus = Value(7 * (rr + r + 1));

        if (rr)
        {
            Square blockSq = s + pawn_push(Us);

            // Adjust bonus based on the king's proximity
            ebonus +=  square_distance(pos.king_square(Them), blockSq) * 5 * rr
                     - square_distance(pos.king_square(Us  ), blockSq) * 2 * rr;

            // If blockSq is not the queening square then consider also a second push
            if (relative_rank(Us, blockSq) != RANK_8)
                ebonus -= square_distance(pos.king_square(Us), blockSq + pawn_push(Us)) * rr;

            // If the pawn is free to advance, then increase the bonus
            if (pos.empty(blockSq))
            {
                // If there is a rook or queen attacking/defending the pawn from behind,
                // consider all the squaresToQueen. Otherwise consider only the squares
                // in the pawn's path attacked or occupied by the enemy.
                defendedSquares = unsafeSquares = squaresToQueen = forward_bb(Us, s);

                Bitboard bb = forward_bb(Them, s) & pos.pieces(ROOK, QUEEN) & pos.attacks_from<ROOK>(s);

                if (!(pos.pieces(Us) & bb))
                    defendedSquares &= ei.attackedBy[Us][ALL_PIECES];

                if (!(pos.pieces(Them) & bb))
                    unsafeSquares &= ei.attackedBy[Them][ALL_PIECES] | pos.pieces(Them);

                // If there aren't any enemy attacks, assign a big bonus. Otherwise
                // assign a smaller bonus if the block square isn't attacked.
                int k = !unsafeSquares ? 15 : !(unsafeSquares & blockSq) ? 9 : 0;

                // If the path to queen is fully defended, assign a big bonus.
                // Otherwise assign a smaller bonus if the block square is defended.
                if (defendedSquares == squaresToQueen)
                    k += 6;

                else if (defendedSquares & blockSq)
                    k += 4;

                mbonus += k * rr, ebonus += k * rr;
            }
            else if (pos.pieces(Us) & blockSq)
                mbonus += rr * 3 + r * 2 + 3, ebonus += rr + r * 2;
        } // rr != 0

        if (pos.count<PAWN>(Us) < pos.count<PAWN>(Them))
            ebonus += ebonus / 4;

        score += make_score(mbonus, ebonus);
    }

    if (Trace)
        Tracing::write(Tracing::PASSED, Us, apply_weight(score, Weights[PassedPawns]));

    // Add the scores to the middlegame and endgame eval
    return apply_weight(score, Weights[PassedPawns]);
  }


  // evaluate_unstoppable_pawns() scores the most advanced passed pawn. In case
  // both players have no pieces but pawns, this is somewhat related to the
  // possibility that pawns are unstoppable.

  Score evaluate_unstoppable_pawns(Color us, const EvalInfo& ei) {

    Bitboard b = ei.pi->passed_pawns(us);

    return b ? Unstoppable * int(relative_rank(us, frontmost_sq(us, b))) : SCORE_ZERO;
  }


  // evaluate_space() computes the space evaluation for a given side. The
  // space evaluation is a simple bonus based on the number of safe squares
  // available for minor pieces on the central four files on ranks 2--4. Safe
  // squares one, two or three squares behind a friendly pawn are counted
  // twice. Finally, the space bonus is scaled by a weight taken from the
  // material hash table. The aim is to improve play on game opening.
  template<Color Us>
  int evaluate_space(const Position& pos, const EvalInfo& ei) {

    const Color Them = (Us == WHITE ? BLACK : WHITE);

    // Find the safe squares for our pieces inside the area defined by
    // SpaceMask[]. A square is unsafe if it is attacked by an enemy
    // pawn, or if it is undefended and attacked by an enemy piece.
    Bitboard safe =   SpaceMask[Us]
                   & ~pos.pieces(Us, PAWN)
                   & ~ei.attackedBy[Them][PAWN]
                   & (ei.attackedBy[Us][ALL_PIECES] | ~ei.attackedBy[Them][ALL_PIECES]);

    // Find all squares which are at most three squares behind some friendly pawn
    Bitboard behind = pos.pieces(Us, PAWN);
    behind |= (Us == WHITE ? behind >>  8 : behind <<  8);
    behind |= (Us == WHITE ? behind >> 16 : behind << 16);

    // Since SpaceMask[Us] is fully on our half of the board
    assert(unsigned(safe >> (Us == WHITE ? 32 : 0)) == 0);

    // Count safe + (behind & safe) with a single popcount
    return popcount<Full>((Us == WHITE ? safe << 32 : safe >> 32) | (behind & safe));
  }


  // do_evaluate() is the evaluation entry point, called directly from evaluate()

  template<bool Trace>
  Value do_evaluate(const Position& pos) {

    assert(!pos.checkers());

    EvalInfo ei;
    Score score, mobility[2] = { SCORE_ZERO, SCORE_ZERO };
    Thread* thisThread = pos.this_thread();

    // Initialize score by reading the incrementally updated scores included
    // in the position object (material + piece square tables).
    // Score is computed from the point of view of white.
    score = pos.psq_score();

    // Probe the material hash table
    ei.mi = Material::probe(pos, thisThread->materialTable, thisThread->endgames);
    score += ei.mi->material_value();

    // If we have a specialized evaluation function for the current material
    // configuration, call it and return.
    if (ei.mi->specialized_eval_exists())
        return ei.mi->evaluate(pos) + Eval::Tempo;

    // Probe the pawn hash table
    ei.pi = Pawns::probe(pos, thisThread->pawnsTable);
    score += apply_weight(ei.pi->pawns_value(), Weights[PawnStructure]);

    // Initialize attack and king safety bitboards
    init_eval_info<WHITE>(pos, ei);
    init_eval_info<BLACK>(pos, ei);

    ei.attackedBy[WHITE][ALL_PIECES] |= ei.attackedBy[WHITE][KING];
    ei.attackedBy[BLACK][ALL_PIECES] |= ei.attackedBy[BLACK][KING];

    // Do not include in mobility squares protected by enemy pawns or occupied by our pawns or king
    Bitboard mobilityArea[] = { ~(ei.attackedBy[BLACK][PAWN] | pos.pieces(WHITE, PAWN, KING)),
                                ~(ei.attackedBy[WHITE][PAWN] | pos.pieces(BLACK, PAWN, KING)) };

    // Evaluate pieces and mobility
    score += evaluate_pieces<KNIGHT, WHITE, Trace>(pos, ei, mobility, mobilityArea);
    score += apply_weight(mobility[WHITE] - mobility[BLACK], Weights[Mobility]);

    // Evaluate kings after all other pieces because we need complete attack
    // information when computing the king safety evaluation.
    score +=  evaluate_king<WHITE, Trace>(pos, ei)
            - evaluate_king<BLACK, Trace>(pos, ei);

    // Evaluate tactical threats, we need full attack information including king
    score +=  evaluate_threats<WHITE, Trace>(pos, ei)
            - evaluate_threats<BLACK, Trace>(pos, ei);

    // Evaluate passed pawns, we need full attack information including king
    score +=  evaluate_passed_pawns<WHITE, Trace>(pos, ei)
            - evaluate_passed_pawns<BLACK, Trace>(pos, ei);

    // If both sides have only pawns, score for potential unstoppable pawns
    if (!pos.non_pawn_material(WHITE) && !pos.non_pawn_material(BLACK))
        score +=  evaluate_unstoppable_pawns(WHITE, ei)
                - evaluate_unstoppable_pawns(BLACK, ei);

    // Evaluate space for both sides, only in middlegame
    if (ei.mi->space_weight())
    {
        int s = evaluate_space<WHITE>(pos, ei) - evaluate_space<BLACK>(pos, ei);
        score += apply_weight(s * ei.mi->space_weight(), Weights[Space]);
    }

    // Scale winning side if position is more drawish than it appears
    Color strongSide = eg_value(score) > VALUE_DRAW ? WHITE : BLACK;
    ScaleFactor sf = ei.mi->scale_factor(pos, strongSide);

    // If we don't already have an unusual scale factor, check for certain
    // types of endgames, and use a lower scale for those.
    if (    ei.mi->game_phase() < PHASE_MIDGAME
        && (sf == SCALE_FACTOR_NORMAL || sf == SCALE_FACTOR_ONEPAWN))
    {
        if (pos.opposite_bishops()) {
            // Ignoring any pawns, do both sides only have a single bishop and no
            // other pieces?
            if (   pos.non_pawn_material(WHITE) == BishopValueMg
                && pos.non_pawn_material(BLACK) == BishopValueMg)
            {
                // Check for KBP vs KB with only a single pawn that is almost
                // certainly a draw or at least two pawns.
                bool one_pawn = (pos.count<PAWN>(WHITE) + pos.count<PAWN>(BLACK) == 1);
                sf = one_pawn ? ScaleFactor(8) : ScaleFactor(32);
            }
            else
                // Endgame with opposite-colored bishops, but also other pieces. Still
                // a bit drawish, but not as drawish as with only the two bishops.
                 sf = ScaleFactor(50 * sf / SCALE_FACTOR_NORMAL);
        } else if (    abs(eg_value(score)) <= BishopValueEg
                   &&  ei.pi->pawn_span(strongSide) <= 1
                   && !pos.pawn_passed(~strongSide, pos.king_square(~strongSide))) {
            // Endings where weaker side can be place his king in front of the opponent's pawns are drawish.
            sf = ScaleFactor(ScalePawnSpan[ei.pi->pawn_span(strongSide)]);
        }
    }

    // Interpolate between a middlegame and a (scaled by 'sf') endgame score
    Value v =  mg_value(score) * int(ei.mi->game_phase())
             + eg_value(score) * int(PHASE_MIDGAME - ei.mi->game_phase()) * sf / SCALE_FACTOR_NORMAL;

    v /= int(PHASE_MIDGAME);

    // In case of tracing add all single evaluation contributions for both white and black
    if (Trace)
    {
        Tracing::write(Tracing::MATERIAL, pos.psq_score());
        Tracing::write(Tracing::IMBALANCE, ei.mi->material_value());
        Tracing::write(PAWN, ei.pi->pawns_value());
        Tracing::write(Tracing::MOBILITY, apply_weight(mobility[WHITE], Weights[Mobility])
                                        , apply_weight(mobility[BLACK], Weights[Mobility]));
        Score w = ei.mi->space_weight() * evaluate_space<WHITE>(pos, ei);
        Score b = ei.mi->space_weight() * evaluate_space<BLACK>(pos, ei);
        Tracing::write(Tracing::SPACE, apply_weight(w, Weights[Space]), apply_weight(b, Weights[Space]));
        Tracing::write(Tracing::TOTAL, score);
        Tracing::ei = ei;
        Tracing::sf = sf;
    }

    return (pos.side_to_move() == WHITE ? v : -v) + Eval::Tempo;
  }


  // Tracing function definitions

  double Tracing::to_cp(Value v) { return double(v) / PawnValueEg; }

  void Tracing::write(int idx, Color c, Score s) { scores[c][idx] = s; }

  void Tracing::write(int idx, Score w, Score b) {

    write(idx, WHITE, w);
    write(idx, BLACK, b);
  }

  void Tracing::print(std::stringstream& ss, const char* name, int idx) {

    Score wScore = scores[WHITE][idx];
    Score bScore = scores[BLACK][idx];

    switch (idx) {
    case MATERIAL: case IMBALANCE: case PAWN: case TOTAL:
        ss << std::setw(15) << name << " |   ---   --- |   ---   --- | "
           << std::setw(5)  << to_cp(mg_value(wScore - bScore)) << " "
           << std::setw(5)  << to_cp(eg_value(wScore - bScore)) << " \n";
        break;
    default:
        ss << std::setw(15) << name << " | " << std::noshowpos
           << std::setw(5)  << to_cp(mg_value(wScore)) << " "
           << std::setw(5)  << to_cp(eg_value(wScore)) << " | "
           << std::setw(5)  << to_cp(mg_value(bScore)) << " "
           << std::setw(5)  << to_cp(eg_value(bScore)) << " | "
           << std::setw(5)  << to_cp(mg_value(wScore - bScore)) << " "
           << std::setw(5)  << to_cp(eg_value(wScore - bScore)) << " \n";
    }
  }

  std::string Tracing::do_trace(const Position& pos) {

    std::memset(scores, 0, sizeof(scores));

    Value v = do_evaluate<true>(pos);
    v = pos.side_to_move() == WHITE ? v : -v; // White's point of view

    std::stringstream ss;
    ss << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2)
       << "      Eval term |    White    |    Black    |    Total    \n"
       << "                |   MG    EG  |   MG    EG  |   MG    EG  \n"
       << "----------------+-------------+-------------+-------------\n";

    print(ss, "Material", MATERIAL);
    print(ss, "Imbalance", IMBALANCE);
    print(ss, "Pawns", PAWN);
    print(ss, "Knights", KNIGHT);
    print(ss, "Bishops", BISHOP);
    print(ss, "Rooks", ROOK);
    print(ss, "Queens", QUEEN);
    print(ss, "Mobility", MOBILITY);
    print(ss, "King safety", KING);
    print(ss, "Threats", THREAT);
    print(ss, "Passed pawns", PASSED);
    print(ss, "Space", SPACE);

    ss << "----------------+-------------+-------------+-------------\n";
    print(ss, "Total", TOTAL);

    ss << "\nTotal Evaluation: " << to_cp(v) << " (white side)\n";

    return ss.str();
  }

} // namespace


namespace Eval {

  /// evaluate() is the main evaluation function. It returns a static evaluation
  /// of the position always from the point of view of the side to move.

  Value evaluate(const Position& pos) {
    return do_evaluate<false>(pos);
  }


  /// trace() is like evaluate(), but instead of returning a value, it returns
  /// a string (suitable for outputting to stdout) that contains the detailed
  /// descriptions and values of each evaluation term. It's mainly used for
  /// debugging.
  std::string trace(const Position& pos) {
    return Tracing::do_trace(pos);
  }


  /// init() computes evaluation weights from the corresponding UCI parameters
  /// and setup king tables.

  void init() {

    const double MaxSlope = 30;
    const double Peak = 1280;

    for (int t = 0, i = 1; i < 100; ++i)
    {
        t = int(std::min(Peak, std::min(0.4 * i * i, t + MaxSlope)));
        KingDanger[i] = apply_weight(make_score(t, 0), Weights[KingSafety]);
    }
  }

} // namespace Eval
