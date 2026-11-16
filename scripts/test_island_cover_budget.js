// RT-1 standalone proof. No CMake/shared build, network, client, or configuration access.
// Usage: node test_island_cover_budget.js TREE NEW_EMPTY_OUTPUT red|green [vcvars64.bat]
'use strict';
const fs = require('fs');
const path = require('path');
const cp = require('child_process');
const zlib = require('node:zlib');

function check(Value, Message) { if(!Value) throw new Error(Message); }
function crc32(Data) {
	let C = 0xffffffff;
	for(const Byte of Data) {
		C ^= Byte;
		for(let I = 0; I < 8; ++I) C = (C >>> 1) ^ ((C & 1) ? 0xedb88320 : 0);
	}
	return (C ^ 0xffffffff) >>> 0;
}
function chunk(Type, Data) {
	const Head = Buffer.alloc(8), Tail = Buffer.alloc(4);
	Head.writeUInt32BE(Data.length); Head.write(Type, 4, 'ascii');
	Tail.writeUInt32BE(crc32(Buffer.concat([Head.subarray(4), Data])));
	return Buffer.concat([Head, Data, Tail]);
}
function png(W, H, Depth, Color, Interlace, Raw, Extras = [], Split = false, Cgbi = false) {
	const IHDR = Buffer.alloc(13);
	IHDR.writeUInt32BE(W); IHDR.writeUInt32BE(H, 4);
	IHDR[8] = Depth; IHDR[9] = Color; IHDR[12] = Interlace;
	const Compressed = Cgbi ? zlib.deflateRawSync(Raw) : zlib.deflateSync(Raw);
	const IDAT = Split ? [chunk('IDAT', Compressed.subarray(0, 3)), chunk('IDAT', Compressed.subarray(3))] : [chunk('IDAT', Compressed)];
	return Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]), ...(Cgbi ? [chunk('CgBI', Buffer.alloc(4))] : []), chunk('IHDR', IHDR), ...Extras, ...IDAT, chunk('IEND', Buffer.alloc(0))]);
}
function samples(X, Y, Depth, Color, Uniform) {
	const Max = (1 << Depth) - 1;
	const V = (X * 17 + Y * 29) & Max;
	const Counts = {0:1,2:3,3:1,4:2,6:4};
	return Array.from({length:Counts[Color]}, (_, C) => Uniform ? 0 : Color === 3 ? (X + Y) & Max : (V + C * 37) & Max);
}
function makeRaw(W, H, Depth, Color, Interlace, Uniform = false) {
	const Passes = Interlace ? [[0,0,8,8],[4,0,8,8],[0,4,4,8],[2,0,4,4],[0,2,2,4],[1,0,2,2],[0,1,1,2]] : [[0,0,1,1]];
	const Parts = [];
	for(const [X0,Y0,DX,DY] of Passes) {
		const PW = Math.max(0, Math.ceil((W-X0)/DX)), PH = Math.max(0, Math.ceil((H-Y0)/DY));
		if(!PW || !PH) continue;
		const Channels = {0:1,2:3,3:1,4:2,6:4}[Color];
		const RowBytes = Math.ceil(PW * Channels * Depth / 8);
		const Pass = Buffer.alloc((RowBytes + 1) * PH);
		for(let PY = 0; PY < PH; ++PY) {
			let Bit = 0;
			const Offset = PY * (RowBytes + 1) + 1;
			for(let PX = 0; PX < PW; ++PX) for(const V of samples(X0+PX*DX, Y0+PY*DY, Depth, Color, Uniform)) {
				if(Depth === 16) { Pass.writeUInt16BE(V, Offset + Bit/8); Bit += 16; }
				else if(Depth === 8) { Pass[Offset + Bit/8] = V; Bit += 8; }
				else { Pass[Offset + (Bit >>> 3)] |= V << (8-Depth-(Bit & 7)); Bit += Depth; }
			}
		}
		Parts.push(Pass);
	}
	return Buffer.concat(Parts);
}
function expected(W, H, Depth, Color, Uniform = false) {
	const Out = Buffer.alloc(W*H*4), Max = (1 << Depth) - 1;
	for(let Y = 0; Y < H; ++Y) for(let X = 0; X < W; ++X) {
		let S = samples(X,Y,Depth,Color,Uniform).map(V => Depth === 16 ? V >>> 8 : Depth < 8 && Color === 0 ? V*255/Max : V);
		if(Color === 0) S = [S[0],S[0],S[0],255];
		else if(Color === 2) S.push(255);
		else if(Color === 3) S = [S[0]*17,255-S[0]*17,S[0]*9,255];
		else if(Color === 4) S = [S[0],S[0],S[0],S[1]];
		for(let C = 0; C < 4; ++C) Out[(Y*W+X)*4+C] = S[C];
	}
	return Out;
}
function grayJpeg() {
	function marker(Code, Data) { const H = Buffer.from([255,Code,0,0]); H.writeUInt16BE(Data.length+2,2); return Buffer.concat([H,Data]); }
	// One 8x8 grayscale block. DC=0 and AC=EOB, both Huffman codes are one zero bit.
	const Counts = Buffer.from([1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]);
	return Buffer.concat([Buffer.from([255,216]), marker(219,Buffer.concat([Buffer.from([0]),Buffer.alloc(64,1)])), marker(192,Buffer.from([8,0,8,0,8,1,1,17,0])), marker(196,Buffer.concat([Buffer.from([0]),Counts,Buffer.from([0,16]),Counts,Buffer.from([0])])), marker(218,Buffer.from([1,1,0,0,63,0])), Buffer.from([63,255,217])]);
}
function extractCover(Island, HeaderPath) {
	const Start = Island.indexOf('void CoverDecodeMain()');
	const End = Island.indexOf('void StartCoverDecodeThread()',Start);
	check(Start >= 0 && End > Start,'cover function not found');
	const Cover = Island.slice(Start,End).replace(/\r\n/g,'\n');
	const Valid = Cover.match(/auto ValidDimensions = \[\]\(int W, int H\) \{[\s\S]*?\n\t\t\};/);
	const Constants = Island.split(/\r?\n/).filter(L => /static constexpr .*COVER_MAX_(BYTES|DIMENSION|PIXELS)\s*=/.test(L)).join('\n');
	const InputStart = Cover.indexOf('if(Png.empty()');
	const Success = Cover.indexOf('Image.m_Width',InputStart);
	const Else = Cover.lastIndexOf('\n\t\t\t\t\telse\n\t\t\t\t\t{',Success);
	check(Valid && Constants.split('\n').length === 3 && InputStart >= 0 && Else > InputStart,'cover extraction changed; refuse approximate proof');
	const Prefix = Cover.slice(InputStart,Else);
	return `#include "${HeaderPath.replace(/\\/g,'/')}"\n${Constants}\nstatic unsigned char *DecodeCover(const std::vector<uint8_t> &Png, int &IW, int &IH, int &IChannels, bool &Failed)\n{\n${Valid[0]}\nstd::string Reason;\n${Prefix}\n}}\nreturn pPixels;\n}\n`;
}
function main() {
	const [TreeArg, OutputArg, Mode, VcArg] = process.argv.slice(2);
	check(TreeArg && OutputArg && ['red','green'].includes(Mode),'usage: TREE NEW_EMPTY_OUTPUT red|green [vcvars64.bat]');
	const Tree = path.resolve(TreeArg), Output = path.resolve(OutputArg);
	check(!fs.existsSync(Output),'output must be a new directory');
	check(!Output.startsWith(Tree + path.sep),'output must be outside source/shared builds');
	for(const Dir of ['fixtures','scripts','compiler','snapshots']) fs.mkdirSync(path.join(Output,Dir),{recursive:true});
	const Component = path.join(Tree,'src/game/client/components/tclient');
	const Island = fs.readFileSync(path.join(Component,'island_music.cpp'),'utf8');
	fs.copyFileSync(path.join(Component,'island_music.cpp'),path.join(Output,'snapshots/island_music.cpp'));
	fs.copyFileSync(path.join(Component,'stb_image.h'),path.join(Output,'snapshots/stb_image_snapshot.h'));
	const FixtureDir = path.join(Output,'fixtures');
	const Manifest = [];
	function add(Name, Data, Accept, W, H, Expected) {
		fs.writeFileSync(path.join(FixtureDir,Name),Data);
		Manifest.push(`${Name} ${Accept?1:0} ${W} ${H}`);
		if(Expected) fs.writeFileSync(path.join(FixtureDir,Name+'.rgba'),Expected);
	}
	// Reproduce the confirmed fixture byte-for-byte; do not depend on a personal TEMP path.
	const BombRaw = Buffer.alloc(8*1024*1024); BombRaw[4] = 255;
	const BombIHDR = Buffer.alloc(13); BombIHDR.writeUInt32BE(1); BombIHDR.writeUInt32BE(1,4); BombIHDR[8]=8; BombIHDR[9]=6;
	const Confirmed = Buffer.concat([Buffer.from([137,80,78,71,13,10,26,10]),chunk('IHDR',BombIHDR),chunk('IDAT',zlib.deflateSync(BombRaw,{level:9})),chunk('IEND',Buffer.alloc(0))]);
	check(Confirmed.length === 8222,'confirmed fixture must be exactly 8222 bytes');
	check(require('node:crypto').createHash('sha256').update(Confirmed).digest('hex') === '7d1e14fed8761d74fbee61d8ad7075de3eed052cdb15a60049130a891c36e02a','confirmed fixture hash differs');
	const IDAT = []; let Offset = 8;
	while(Offset+12 <= Confirmed.length) { const Size = Confirmed.readUInt32BE(Offset), Type = Confirmed.toString('ascii',Offset+4,Offset+8); check(Offset+12+Size <= Confirmed.length,'bad confirmed chunk'); if(Type === 'IDAT') IDAT.push(Confirmed.subarray(Offset+8,Offset+8+Size)); Offset += Size+12; }
	const Inflated = zlib.inflateSync(Buffer.concat(IDAT),{maxOutputLength:32*1024*1024});
	check(Confirmed.readUInt32BE(16) === 1 && Confirmed.readUInt32BE(20) === 1 && Inflated.length > 4*1024*1024,'wrong confirmed counterexample');
	add('confirmed-8222.png',Confirmed,false,1,1);
	add('stored-overrun.png',png(1,1,8,6,0,Buffer.alloc(100),[],false),false,1,1);
	// Truly uncompressed DEFLATE blocks exercise the separate stored-block output guard.
	const Stored = zlib.deflateSync(Buffer.alloc(100),{level:0});
	const StoredPng = png(1,1,8,6,0,Buffer.alloc(100));
	let P = 8; const Chunks = [StoredPng.subarray(0,8)];
	while(P < StoredPng.length) { const N = StoredPng.readUInt32BE(P), T = StoredPng.toString('ascii',P+4,P+8); Chunks.push(T === 'IDAT' ? chunk('IDAT',Stored) : StoredPng.subarray(P,P+12+N)); P += 12+N; }
	add('stored-block-overrun.png',Buffer.concat(Chunks),false,1,1);
	add('one-byte-overrun.png',png(1,1,8,6,0,Buffer.alloc(6)),false,1,1);
	add('not-enough-pixels.png',png(1,1,8,6,0,Buffer.alloc(4)),false,1,1);
	for(const [Color,Depths] of [[0,[1,2,4,8,16]],[2,[8,16]],[3,[1,2,4,8]],[4,[8,16]],[6,[8,16]]]) {
		for(const Depth of Depths) for(const Interlace of [0,1]) {
			const Name = `${Color===6?'rgba':'color'+Color}${Depth}-${Interlace?'adam7':'plain'}.png`;
			const Extras = [];
			if(Color === 3) { const Palette = Buffer.alloc((1 << Depth)*3); for(let I=0;I<(1<<Depth);++I){Palette[I*3]=I*17;Palette[I*3+1]=255-I*17;Palette[I*3+2]=I*9;} Extras.push(chunk('PLTE',Palette)); }
			add(Name,png(13,11,Depth,Color,Interlace,makeRaw(13,11,Depth,Color,Interlace),Extras,true),true,13,11,expected(13,11,Depth,Color));
		}
	}
	for(const [W,H,Depth,Color] of [[1,1,16,6],[1,17,1,0],[17,1,2,0],[3,2,4,0],[2048,2048,16,6]]) {
		add(`edge-${W}x${H}-${Depth}.png`,png(W,H,Depth,Color,1,makeRaw(W,H,Depth,Color,1,true)),true,W,H,expected(W,H,Depth,Color,true));
	}
	add('invalid-filter.png',png(1,1,8,6,0,Buffer.from([5,0,0,0,0])),false,1,1);
	add('cgbi-plain.png',png(1,1,8,6,0,Buffer.alloc(5),[],false,true),true,1,1,Buffer.alloc(4));
	add('gray.jpg',grayJpeg(),true,8,8,Buffer.from(Array.from({length:64},()=>[128,128,128,255]).flat()));
	add('dimension-over-limit.png',png(2049,1,8,6,0,Buffer.alloc(1)),false,0,0);
	fs.writeFileSync(path.join(FixtureDir,'manifest.txt'),Manifest.join('\n')+'\n');
	fs.writeFileSync(path.join(Output,'scripts/cover_decode_generated.h'),extractCover(Island,path.join(Output,'snapshots/stb_image_snapshot.h')));
	fs.copyFileSync(path.join(Tree,'src/test/island_cover_budget.cpp'),path.join(Output,'scripts/island_cover_budget.cpp'));
	const Vc = VcArg || 'C:/VS2026/BuildTools/VC/Auxiliary/Build/vcvars64.bat';
	const Cmd = path.join(Output,'scripts/build.cmd');
	fs.writeFileSync(Cmd,`@call "${Vc}" >nul\r\n@if errorlevel 1 exit /b 2\r\n@cl /nologo /utf-8 /std:c++17 /EHsc /W4 /O2 /MT /Fo"${path.join(Output,'compiler/test.obj')}" /Fe"${path.join(Output,'compiler/test.exe')}" "${path.join(Output,'scripts/island_cover_budget.cpp')}"\r\n@exit /b %errorlevel%\r\n`);
	const Build = cp.spawnSync('cmd.exe',['/d','/s','/c',`"${Cmd}"`],{cwd:path.join(Output,'scripts'),windowsVerbatimArguments:true,encoding:'utf8',timeout:120000});
	fs.writeFileSync(path.join(Output,'compiler/build.log'),(Build.stdout||'')+(Build.stderr||''));
	check(Build.status === 0,`standalone compile failed: ${Output}/compiler/build.log (status ${Build.status})`);
	const Run = cp.spawnSync(path.join(Output,'compiler/test.exe'),[FixtureDir,Mode],{cwd:path.join(Output,'scripts'),encoding:'utf8',timeout:120000});
	fs.writeFileSync(path.join(Output,'test.log'),(Run.stdout||'')+(Run.stderr||''));
	console.log(`output=${Output} tree=${Tree} mode=${Mode} fixture_bytes=8222 zlib_output_bytes=${Inflated.length}`);
	console.log((Run.stdout||'').trim());
	check(Run.status === 0,`regression failed: ${Output}/test.log (status ${Run.status})`);
}
try { main(); } catch(Error) { console.error(Error.message); process.exitCode = 1; }
