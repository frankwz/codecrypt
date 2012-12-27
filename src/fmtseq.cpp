
/*
 * This file is part of Codecrypt.
 *
 * Codecrypt is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version.
 *
 * Codecrypt is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Codecrypt. If not, see <http://www.gnu.org/licenses/>.
 */

#include "fmtseq.h"
#include "arcfour.h"

#include <iostream>

using namespace fmtseq;

void prepare_keygen (arcfour<byte>& kg, const std::vector<byte>&SK, uint idx)
{
	kg.clear();
	kg.init (8);
	kg.load_key (SK);
	std::vector<byte>tmp;
	while (idx) {
		tmp.push_back (idx & 0xff);
		idx >>= 8;
	}
	tmp.resize (16, 0); //prevent chaining to other numbers
	kg.load_key (tmp);
}

static void add_zero_checksum (bvector& v)
{

	uint s = v.size();
	if (!s) return;

	uint z = s - v.hamming_weight(); //0's instead of 1's

	v.resize (fmtseq_commitments (s) );
	while (z) {
		v[s] = z & 1;
		z >>= 1;
		++s;
	}
}

static void alloc_exist (privkey&priv)
{
	priv.exist.resize (priv.l);
	uint ts = (1 << (priv.h + 1) ) - 2;
	for (uint i = 0; i < priv.l; ++i)
		priv.exist[i].resize (ts);
}

static void store_exist (privkey&priv, const privkey::tree_stk_item&i)
{
	uint level = i.level / priv.h;
	if (level >= priv.l) return; //top node
	uint sublevel = priv.h - (i.level % priv.h);
	if (i.pos >= (1 << sublevel) ) return; //too far right

	priv.exist[level][i.pos + (1 << sublevel) - 2] = i.item;
}

static void update_trees (privkey&priv, hash_func&hf)
{
	//TODO
}

int fmtseq::generate (pubkey&pub, privkey&priv,
                      prng&rng, hash_func&hf,
                      uint hs, uint h, uint l)
{
	uint i, j;

	/*
	 * first off, generate a secret key for commitment generator.
	 * exactly THIS gives the amount of all possible FMTseq privkeys.
	 *
	 * in our case it's around 2^2048, which is Enough.
	 */
	priv.SK.resize (1 << 8);
	for (i = 0; i < (1 << 8); ++i) {
		priv.SK[i] = rng.random (1 << 8);
	}

	priv.h = h;
	priv.l = l;
	priv.hs = hs;
	priv.sigs_used = 0;

	std::vector<privkey::tree_stk_item> stk;
	stk.reserve (h * l + 1);

	uint sigs = 1 << (h * l);

	uint commitments = fmtseq_commitments (hs);

	arcfour<byte> generator;
	std::vector<byte> x, y, Y;

	x.resize (hf.size() );
	y.resize (hf.size() );

	alloc_exist (priv);

	for (i = 0; i < sigs; ++i) {
		//generate commitments and concat publics into Y
		Y.clear();
		Y.reserve (commitments * hf.size() );
		prepare_keygen (generator, priv.SK, i);
		for (j = 0; j < commitments; ++j) {
			generator.gen (hf.size(), x);
			y = hf (x);
			Y.insert (Y.end(), y.begin(), y.end() );
		}

		stk.push_back (privkey::tree_stk_item (0, i, hf (Y) ) );
		store_exist (priv, stk.back() );

		//try squashing the stack
		for (;;) {
			if (stk.size() < 2) break;
			if ( (stk.end() - 1)->level != (stk.end() - 2)->level) break;

			Y.clear();
			Y.insert (Y.end(),
			          (stk.end() - 2)->item.begin(),
			          (stk.end() - 2)->item.end() );
			Y.insert (Y.end(),
			          (stk.end() - 1)->item.begin(),
			          (stk.end() - 1)->item.end() );
			uint l = stk.back().level + 1;
			uint p = stk.back().pos / 2;
			stk.pop_back();
			stk.pop_back();
			stk.push_back (privkey::tree_stk_item (l, p, hf (Y) ) );
			store_exist (priv, stk.back() );
		}
	}

	//now there's the public verification key available in the stack.
	pub.check = stk.back().item;
	pub.H = h * l;
	pub.hs = hs;

	return 0;
}

/*
 * SIGNATURE STRUCTURE
 * is variable, but following stuff is concatenated exactly in this order:
 *
 * - private/public commitments (less than size+log2(size) in bits times hash
 *   size) in the "natural" left-to-right order (or from first to last bits of
 *   hash.  Checksum goes last, with least significant bit first). Private
 *   commitment goes whenever there's 1 in message, public on 0.
 * - h*l hashes of verification chain, from bottom to top, h*l times hash size
 * - i (so that we can guess left/right concatenation before hashing) stored as
 *   H-bit number in little endian.
 *   TODO Why cannot the tree be just XORed together? would be WAY simpler!
 *
 * summed up:
 *
 * Sig=(x0, y1, x2, x3, y4, ..... , xComm-1,path0,path1,...,pathH-1, i)
 *
 */

#include "ios.h"

int privkey::sign (const bvector& hash, bvector& sig, hash_func& hf)
{
	if (hash.size() != hash_size() ) return 2;
	if (!sigs_remaining() ) return 2;

	uint commitments = fmtseq_commitments (hs);

	bvector M2 = hash;
	add_zero_checksum (M2);

	std::vector<byte> Sig, t;
	uint i;

	Sig.reserve (hf.size() * (commitments + h * l) );
	//first, compute the commitments and push them to the signature
	arcfour<byte> generator;
	prepare_keygen (generator, SK, sigs_used);
	for (i = 0; i < commitments; ++i) {
		//generate x_i
		generator.gen (hf.size(), t);

		//if it's 0, publish y_i, else publish x_i
		if (!M2[i]) t = hf (t);

		//append it to signature
		Sig.insert (Sig.end(), t.begin(), t.end() );
	}

	//now retrieve the authentication path
	uint pos = sigs_used;
	uint exlev, expos, exid;
	for (i = 0; i < h * l; ++i) {
		exid = i / h;
		exlev = h - (i % h);
		//flip the last bit of pos so it gets the neighbor
		expos = (pos ^ 1) % (1 << exlev);
		Sig.insert (Sig.end(),
		            exist[exid][expos + (1 << exlev) - 2].begin(),
		            exist[exid][expos + (1 << exlev) - 2].end() );
		pos >>= 1;
	}

	//prepare the signature
	sig.clear();
	sig.resize (signature_size (hf), 0);

	//convert to bits
	uint sig_no_start = (commitments + h * l) * hf.size() * 8;
	for (i = 0; i < sig_no_start; ++i)
		sig[i] = 1 & (Sig[i / 8] >> (i % 8) );

	//append signature number
	pos = sigs_used;
	for (i = 0; i < h * l; ++i) {
		sig[i + sig_no_start] = pos & 1;
		pos >>= 1;
	}

	//move on to the next signature
	++sigs_used;
	//update the cache
	update_trees (*this, hf);
	return 0;
}

int pubkey::verify (const bvector& sig, const bvector& hash, hash_func& hf)
{
	uint i, j;
	if (sig.size() != signature_size (hf) ) return 2;
	if (hash.size() != hash_size() ) return 2;

	uint commitments = fmtseq_commitments (hs);

	bvector M2 = hash;
	add_zero_checksum (M2);
	if (M2.size() != commitments) return 3; //likely internal failure

	//retrieve i
	uint sig_no = 0;
	for (i = sig.size() - 1; i >= (commitments + H) * hf.size() * 8; --i)
		sig_no = (sig_no << 1) + (sig[i] ? 1 : 0);

	std::vector<byte> t, Y;
	std::vector<std::vector<byte> > Sig;

	//split and convert to byte form for convenient hashing
	Sig.resize (commitments + H);
	for (i = 0; i < (commitments + H); ++i) {
		Sig[i].resize (hf.size(), 0);
		for (j = 0; j < hf.size() * 8; ++j)
			if (sig[j + i * hf.size() * 8])
				Sig[i][j / 8] |= (1 << (j % 8) );
	}

	Y.clear();
	for (i = 0; i < commitments; ++i) {
		if (M2[i]) t = hf (Sig[i]); //convert pk_i to sk_i at 1's
		else t = Sig[i]; //else it should already be pk_i
		Y.insert (Y.end(), t.begin(), t.end() ); //append it to Y_i
	}

	//create the leaf
	t = hf (Y);

	//walk the authentication path
	for (i = 0; i < H; ++i) {
		Y.clear();
		Y = Sig[commitments + i];
		if ( (sig_no >> i) & 1) {
			//append path auth from left
			Y.insert (Y.end(), t.begin(), t.end() );
			t = hf (Y);
		} else {
			//append from right
			t.insert (t.end(), Y.begin(), Y.end() );
			t = hf (t);
		}

	}

	if (t == check) return 0; //all went okay
	else return 1;
}