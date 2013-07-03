/**
    biobambam
    Copyright (C) 2009-2013 German Tischler
    Copyright (C) 2011-2013 Genome Research Limited

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/

#include <biobambam/BamBamConfig.hpp>
#include <biobambam/Licensing.hpp>

#include <iomanip>

#include <config.h>

#include <libmaus/bambam/CircularHashCollatingBamDecoder.hpp>
#include <libmaus/bambam/BamToFastqOutputFileSet.hpp>
#include <libmaus/bambam/BamWriter.hpp>
#include <libmaus/util/TempFileRemovalContainer.hpp>
#include <libmaus/util/MemUsage.hpp>
#include <libmaus/bambam/ProgramHeaderLineSet.hpp>

static int getDefaultLevel() { return Z_DEFAULT_COMPRESSION; }

static int getLevel(libmaus::util::ArgInfo const & arginfo)
{
	int const level = arginfo.getValue<int>("level",getDefaultLevel());
	
	switch ( level )
	{
		case Z_NO_COMPRESSION:
		case Z_BEST_SPEED:
		case Z_BEST_COMPRESSION:
		case Z_DEFAULT_COMPRESSION:
			break;
		default:
		{
			::libmaus::exception::LibMausException se;
			se.getStream()
				<< "Unknown compression level, please use"
				<< " level=" << Z_DEFAULT_COMPRESSION << " (default) or"
				<< " level=" << Z_BEST_SPEED << " (fast) or"
				<< " level=" << Z_BEST_COMPRESSION << " (best) or"
				<< " level=" << Z_NO_COMPRESSION << " (no compression)" << std::endl;
			se.finish();
			throw se;
		}
			break;
	}
	
	return level;
}

struct BamToFastQInputFileStream
{
	std::string const fn;
	libmaus::aio::CheckedInputStream::unique_ptr_type CIS;
	std::istream & in;
	
	static libmaus::aio::CheckedInputStream::unique_ptr_type openFile(std::string const & fn)
	{
		return UNIQUE_PTR_MOVE(libmaus::aio::CheckedInputStream::unique_ptr_type(new libmaus::aio::CheckedInputStream(fn)));
	}
	
	BamToFastQInputFileStream(libmaus::util::ArgInfo const & arginfo)
	: fn(arginfo.getValue<std::string>("filename","-")),
	  CIS(
		(fn != "-") ? UNIQUE_PTR_MOVE(openFile(fn)) : UNIQUE_PTR_MOVE(libmaus::aio::CheckedInputStream::unique_ptr_type())
	), in((fn != "-") ? (*CIS) : std::cin) {}

	BamToFastQInputFileStream(std::string const & rfn)
	: fn(rfn), CIS(
		(fn != "-") ? UNIQUE_PTR_MOVE(openFile(fn)) : UNIQUE_PTR_MOVE(libmaus::aio::CheckedInputStream::unique_ptr_type())
	), in((fn != "-") ? (*CIS) : std::cin) {}
};

template<typename decoder_type>
std::string getModifiedHeaderText(decoder_type const & bamdec, libmaus::util::ArgInfo const & arginfo)
{
	libmaus::bambam::BamHeader const & header = bamdec.getHeader();
	std::string const headertext(header.text);

	// add PG line to header
	std::string const upheadtext = ::libmaus::bambam::ProgramHeaderLineSet::addProgramLine(
		headertext,
		"bamcollate2", // ID
		"bamcollate2", // PN
		arginfo.commandline, // CL
		::libmaus::bambam::ProgramHeaderLineSet(headertext).getLastIdInChain(), // PP
		std::string(PACKAGE_VERSION) // VN			
	);

	return upheadtext;
}

void bamcollate2NonCollating(libmaus::util::ArgInfo const & arginfo, libmaus::bambam::BamAlignmentDecoder & bamdec)
{
	if ( arginfo.getValue<unsigned int>("disablevalidation",0) )
		bamdec.disableValidation();

	libmaus::timing::RealTimeClock rtc; rtc.start();
	uint32_t const excludeflags = libmaus::bambam::BamFlagBase::stringToFlags(arginfo.getValue<std::string>("exclude","SECONDARY"));
	libmaus::bambam::BamAlignment const & algn = bamdec.getAlignment();
	::libmaus::autoarray::AutoArray<uint8_t> T;
	uint64_t cnt = 0;
	unsigned int const verbshift = 20;

	// construct new header
	::libmaus::bambam::BamHeader uphead(getModifiedHeaderText(bamdec,arginfo));
	uphead.changeSortOrder("unknown");
	// construct writer
	libmaus::bambam::BamWriter bamwriter(std::cout,uphead,getLevel(arginfo));
		
	while ( bamdec.readAlignment() )
	{
		uint64_t const precnt = cnt++;
		
		if ( ! (algn.getFlags() & excludeflags) )
			algn.serialise(bamwriter.getStream());

		if ( precnt >> verbshift != cnt >> verbshift )
			std::cerr 
				<< (cnt >> 20) 
				<< "\t"
				<< "\t" << static_cast<double>(cnt)/rtc.getElapsedSeconds() << std::endl;
	}

}

void bamcollate2NonCollating(libmaus::util::ArgInfo const & arginfo)
{
	std::string const inputformat = arginfo.getValue<std::string>("inputformat","bam");
	std::string const inputfilename = arginfo.getValue<std::string>("filename","-");
	uint64_t const numthreads = arginfo.getValue<uint64_t>("threads",0);
	
	if ( inputformat == "bam" )
	{
		BamToFastQInputFileStream bamin(inputfilename);
		if ( numthreads > 0 )
		{
			libmaus::bambam::BamParallelDecoderWrapper bamdecwrap(bamin.in,numthreads);
			bamcollate2NonCollating(arginfo,bamdecwrap.getDecoder());	
		}
		else
		{
			libmaus::bambam::BamDecoder bamdec(bamin.in);
			bamcollate2NonCollating(arginfo,bamdec);
		}
	}
	#if defined(BIOBAMBAM_LIBMAUS_HAVE_IO_LIB)
	else if ( inputformat == "sam" )
	{
		libmaus::bambam::ScramDecoder bamdec(inputfilename,"r","");
		bamcollate2NonCollating(arginfo,bamdec);
	}
	else if ( inputformat == "cram" )
	{
		std::string const reference = arginfo.getValue<std::string>("reference","");
		libmaus::bambam::ScramDecoder bamdec(inputfilename,"rc",reference);
		bamcollate2NonCollating(arginfo,bamdec);
	}
	#endif
	else
	{
		libmaus::exception::LibMausException se;
		se.getStream() << "unknown input format " << inputformat << std::endl;
		se.finish();
		throw se;
	}
		
	std::cout.flush();
}

void bamcollate2Collating(
	libmaus::util::ArgInfo const & arginfo,
	libmaus::bambam::CircularHashCollatingBamDecoder & CHCBD
)
{
	if ( arginfo.getValue<unsigned int>("disablevalidation",0) )
		CHCBD.disableValidation();

	libmaus::bambam::BamToFastqOutputFileSet OFS(arginfo);

	libmaus::bambam::CircularHashCollatingBamDecoder::OutputBufferEntry const * ob = 0;
	
	// number of alignments written to files
	uint64_t cnt = 0;
	// number of bytes written to files
	uint64_t bcnt = 0;
	unsigned int const verbshift = 20;
	libmaus::timing::RealTimeClock rtc; rtc.start();

	// construct new header
	::libmaus::bambam::BamHeader uphead(getModifiedHeaderText(CHCBD,arginfo));
	uphead.changeSortOrder("unknown");
	// construct writer
	libmaus::bambam::BamWriter bamwriter(std::cout,uphead,getLevel(arginfo));
	typedef libmaus::bambam::BamWriter::stream_type out_stream_type;
	out_stream_type & bgzfos = bamwriter.getStream();
	
	while ( (ob = CHCBD.process()) )
	{
		uint64_t const precnt = cnt;
		
		if ( ob->fpair )
		{
			::libmaus::bambam::EncoderBase::putLE<out_stream_type,uint32_t>(bgzfos,ob->blocksizea);
			bgzfos.write(reinterpret_cast<char const *>(ob->Da),ob->blocksizea);
			::libmaus::bambam::EncoderBase::putLE<out_stream_type,uint32_t>(bgzfos,ob->blocksizeb);
			bgzfos.write(reinterpret_cast<char const *>(ob->Db),ob->blocksizeb);

			cnt += 2;
			bcnt += (ob->blocksizea+ob->blocksizeb);
		}
		else if ( ob->fsingle )
		{
			::libmaus::bambam::EncoderBase::putLE<out_stream_type,uint32_t>(bgzfos,ob->blocksizea);
			bgzfos.write(reinterpret_cast<char const *>(ob->Da),ob->blocksizea);

			cnt += 1;
			bcnt += (ob->blocksizea);
		}
		else if ( ob->forphan1 )
		{
			::libmaus::bambam::EncoderBase::putLE<out_stream_type,uint32_t>(bgzfos,ob->blocksizea);
			bgzfos.write(reinterpret_cast<char const *>(ob->Da),ob->blocksizea);

			cnt += 1;
			bcnt += (ob->blocksizea);
		}
		else if ( ob->forphan2 )
		{
			::libmaus::bambam::EncoderBase::putLE<out_stream_type,uint32_t>(bgzfos,ob->blocksizea);
			bgzfos.write(reinterpret_cast<char const *>(ob->Da),ob->blocksizea);

			cnt += 1;
			bcnt += (ob->blocksizea);
		}
		
		if ( precnt >> verbshift != cnt >> verbshift )
		{
			std::cerr 
				<< "[V] "
				<< (cnt >> 20) 
				<< "\t"
				<< (static_cast<double>(bcnt)/(1024.0*1024.0))/rtc.getElapsedSeconds() << "MB/s"
				<< "\t" << static_cast<double>(cnt)/rtc.getElapsedSeconds() << std::endl;
		}
	}
	
	std::cerr << "[V] " << cnt << std::endl;
}

void bamcollate2Collating(libmaus::util::ArgInfo const & arginfo)
{
	uint32_t const excludeflags = libmaus::bambam::BamFlagBase::stringToFlags(arginfo.getValue<std::string>("exclude","SECONDARY"));
	libmaus::util::TempFileRemovalContainer::setup();
	std::string const tmpfilename = arginfo.getValue<std::string>("T",arginfo.getDefaultTmpFileName());
	libmaus::util::TempFileRemovalContainer::addTempFile(tmpfilename);
	std::string const inputformat = arginfo.getValue<std::string>("inputformat","bam");
	std::string const inputfilename = arginfo.getValue<std::string>("filename","-");
	uint64_t const numthreads = arginfo.getValue<uint64_t>("threads",0);

	unsigned int const hlog = arginfo.getValue<unsigned int>("colhlog",18);
	uint64_t const sbs = arginfo.getValueUnsignedNumeric<uint64_t>("colsbs",128ull*1024ull*1024ull);

	if ( inputformat == "bam" )
	{
		BamToFastQInputFileStream bamin(inputfilename);

		if ( numthreads > 0 )
		{
			libmaus::bambam::BamParallelCircularHashCollatingBamDecoder CHCBD(
				bamin.in,
				numthreads,
				tmpfilename,excludeflags,
				false, /* put rank */
				hlog,
				sbs
				);
			bamcollate2Collating(arginfo,CHCBD);
		}
		else
		{
			libmaus::bambam::BamCircularHashCollatingBamDecoder CHCBD(
				bamin.in,
				tmpfilename,excludeflags,
				false, /* put rank */
				hlog,
				sbs
				);
			bamcollate2Collating(arginfo,CHCBD);
		}
	}
	#if defined(BIOBAMBAM_LIBMAUS_HAVE_IO_LIB)
	else if ( inputformat == "sam" )
	{
		libmaus::bambam::ScramCircularHashCollatingBamDecoder CHCBD(inputfilename,"r","",
			tmpfilename,excludeflags,
			false, /* put rank */
			hlog,sbs
		);
		bamcollate2Collating(arginfo,CHCBD);
	}
	else if ( inputformat == "cram" )
	{
		std::string const reference = arginfo.getValue<std::string>("reference","");
		libmaus::bambam::ScramCircularHashCollatingBamDecoder CHCBD(inputfilename,"rc",reference,
			tmpfilename,excludeflags,
			false, /* put rank */
			hlog,sbs
		);
		bamcollate2Collating(arginfo,CHCBD);
	}
	#endif
	else
	{
		libmaus::exception::LibMausException se;
		se.getStream() << "unknown input format " << inputformat << std::endl;
		se.finish();
		throw se;
	}
	
	std::cout.flush();
}

void bamcollate2CollatingRanking(
	libmaus::util::ArgInfo const & arginfo,
	libmaus::bambam::CircularHashCollatingBamDecoder & CHCBD
)
{
	if ( arginfo.getValue<unsigned int>("disablevalidation",0) )
		CHCBD.disableValidation();

	libmaus::bambam::BamToFastqOutputFileSet OFS(arginfo);

	libmaus::bambam::CircularHashCollatingBamDecoder::OutputBufferEntry const * ob = 0;
	
	// number of alignments written to files
	uint64_t cnt = 0;
	// number of bytes written to files
	uint64_t bcnt = 0;
	unsigned int const verbshift = 20;
	libmaus::timing::RealTimeClock rtc; rtc.start();
	::libmaus::autoarray::AutoArray<uint8_t> T;
	libmaus::bambam::BamAlignment algn;

	// construct new header
	::libmaus::bambam::BamHeader uphead(getModifiedHeaderText(CHCBD,arginfo));
	uphead.changeSortOrder("unknown");
	// construct writer
	libmaus::bambam::BamWriter bamwriter(std::cout,uphead,getLevel(arginfo));
	typedef libmaus::bambam::BamWriter::stream_type out_stream_type;
	out_stream_type & bgzfos = bamwriter.getStream();

	libmaus::bambam::BamAuxFilterVector zrtag;
	zrtag.set('Z','R');

	while ( (ob = CHCBD.process()) )
	{
		uint64_t const precnt = cnt;
		
		if ( ob->fpair )
		{
			uint64_t const ranka = libmaus::bambam::BamAlignmentDecoderBase::getRank(ob->Da,ob->blocksizea);
			uint64_t const rankb = libmaus::bambam::BamAlignmentDecoderBase::getRank(ob->Db,ob->blocksizeb);
			
			if ( algn.D.size() < std::max(ob->blocksizea,ob->blocksizeb) )
				algn.D.resize(std::max(ob->blocksizea,ob->blocksizeb));
				
			std::ostringstream nameostr;
			nameostr << ranka << "_" << rankb << "_" << libmaus::bambam::BamAlignmentDecoderBase::getReadName(ob->Da);
			std::string const name = nameostr.str();
			
			std::copy ( ob->Da, ob->Da + ob->blocksizea, algn.D.begin() );
			algn.blocksize = ob->blocksizea;
			algn.replaceName(name.c_str(),name.size());
			algn.filterOutAux(zrtag);
			algn.serialise(bgzfos);
			
			std::copy ( ob->Db, ob->Db + ob->blocksizeb, algn.D.begin() );
			algn.blocksize = ob->blocksizeb;
			algn.replaceName(name.c_str(),name.size());
			algn.filterOutAux(zrtag);
			algn.serialise(bgzfos);
			
			cnt += 2;
		}
		else if ( ob->fsingle )
		{
			uint64_t const ranka = libmaus::bambam::BamAlignmentDecoderBase::getRank(ob->Da,ob->blocksizea);
			
			if ( algn.D.size() < ob->blocksizea )
				algn.D.resize(ob->blocksizea);

				
			std::ostringstream nameostr;
			nameostr << ranka << "_" << libmaus::bambam::BamAlignmentDecoderBase::getReadName(ob->Da);
			std::string const name = nameostr.str();
				
			std::copy ( ob->Da, ob->Da + ob->blocksizea, algn.D.begin() );
			algn.blocksize = ob->blocksizea;
			algn.replaceName(name.c_str(),name.size());
			algn.filterOutAux(zrtag);
			algn.serialise(bgzfos);

			cnt += 1;
		}
		else if ( ob->forphan1 )
		{
			uint64_t const ranka = libmaus::bambam::BamAlignmentDecoderBase::getRank(ob->Da,ob->blocksizea);
			
			if ( algn.D.size() < ob->blocksizea )
				algn.D.resize(ob->blocksizea);

			std::ostringstream nameostr;
			nameostr << ranka << "_" << ranka << "_" << libmaus::bambam::BamAlignmentDecoderBase::getReadName(ob->Da);
			std::string const name = nameostr.str();
				
			std::copy ( ob->Da, ob->Da + ob->blocksizea, algn.D.begin() );
			algn.blocksize = ob->blocksizea;
			algn.replaceName(name.c_str(),name.size());
			algn.filterOutAux(zrtag);
			algn.serialise(bgzfos);

			cnt += 1;
		}
		else if ( ob->forphan2 )
		{
			uint64_t const ranka = libmaus::bambam::BamAlignmentDecoderBase::getRank(ob->Da,ob->blocksizea);
			
			if ( algn.D.size() < ob->blocksizea )
				algn.D.resize(ob->blocksizea);

			std::ostringstream nameostr;
			nameostr << ranka << "_" << ranka << "_" << libmaus::bambam::BamAlignmentDecoderBase::getReadName(ob->Da);
			std::string const name = nameostr.str();
				
			std::copy ( ob->Da, ob->Da + ob->blocksizea, algn.D.begin() );
			algn.blocksize = ob->blocksizea;
			algn.replaceName(name.c_str(),name.size());
			algn.filterOutAux(zrtag);
			algn.serialise(bgzfos);

			cnt += 1;
		}
		
		if ( precnt >> verbshift != cnt >> verbshift )
		{
			std::cerr 
				<< "[V] "
				<< (cnt >> 20) 
				<< "\t"
				<< (static_cast<double>(bcnt)/(1024.0*1024.0))/rtc.getElapsedSeconds() << "MB/s"
				<< "\t" << static_cast<double>(cnt)/rtc.getElapsedSeconds() << std::endl;
		}
	}
	
	std::cerr << "[V] " << cnt << std::endl;
}

void bamcollate2CollatingRanking(libmaus::util::ArgInfo const & arginfo)
{
	uint32_t const excludeflags = libmaus::bambam::BamFlagBase::stringToFlags(arginfo.getValue<std::string>("exclude","SECONDARY"));
	libmaus::util::TempFileRemovalContainer::setup();
	std::string const tmpfilename = arginfo.getValue<std::string>("T",arginfo.getDefaultTmpFileName());
	libmaus::util::TempFileRemovalContainer::addTempFile(tmpfilename);
	std::string const inputformat = arginfo.getValue<std::string>("inputformat","bam");
	std::string const inputfilename = arginfo.getValue<std::string>("filename","-");
	uint64_t const numthreads = arginfo.getValue<uint64_t>("threads",0);

	unsigned int const hlog = arginfo.getValue<unsigned int>("colhlog",18);
	uint64_t const sbs = arginfo.getValueUnsignedNumeric<uint64_t>("colsbs",128ull*1024ull*1024ull);

	if ( inputformat == "bam" )
	{
		BamToFastQInputFileStream bamin(inputfilename);

		if ( numthreads > 0 )
		{
			libmaus::bambam::BamParallelCircularHashCollatingBamDecoder CHCBD(
				bamin.in,
				numthreads,
				tmpfilename,excludeflags,
				true, /* put rank */
				hlog,
				sbs
				);
			bamcollate2CollatingRanking(arginfo,CHCBD);
		}
		else
		{
			libmaus::bambam::BamCircularHashCollatingBamDecoder CHCBD(
				bamin.in,
				tmpfilename,excludeflags,
				true, /* put rank */
				hlog,
				sbs
				);
			bamcollate2CollatingRanking(arginfo,CHCBD);
		}
	}
	#if defined(BIOBAMBAM_LIBMAUS_HAVE_IO_LIB)
	else if ( inputformat == "sam" )
	{
		libmaus::bambam::ScramCircularHashCollatingBamDecoder CHCBD(inputfilename,"r","",
			tmpfilename,excludeflags,
			true, /* put rank */
			hlog,sbs
		);
		bamcollate2CollatingRanking(arginfo,CHCBD);
	}
	else if ( inputformat == "cram" )
	{
		std::string const reference = arginfo.getValue<std::string>("reference","");
		libmaus::bambam::ScramCircularHashCollatingBamDecoder CHCBD(inputfilename,"rc",reference,
			tmpfilename,excludeflags,
			true, /* put rank */
			hlog,sbs
		);
		bamcollate2CollatingRanking(arginfo,CHCBD);
	}
	#endif
	else
	{
		libmaus::exception::LibMausException se;
		se.getStream() << "unknown input format " << inputformat << std::endl;
		se.finish();
		throw se;
	}
	
	std::cout.flush();
}

void bamcollate2(libmaus::util::ArgInfo const & arginfo)
{
	switch ( arginfo.getValue<uint64_t>("collate",1) )
	{
		case 0:
			bamcollate2NonCollating(arginfo);
			break;
		case 1:
			bamcollate2Collating(arginfo);
			break;
		case 2:		
			bamcollate2CollatingRanking(arginfo);
			break;
		default:
		{
			libmaus::exception::LibMausException se;
			se.getStream() << "unknown collate argument " << arginfo.getValue<uint64_t>("collate",1) << std::endl;
			se.finish();
			throw se;
		}
	}
}

int main(int argc, char * argv[])
{
	try
	{
		libmaus::timing::RealTimeClock rtc; rtc.start();
		
		::libmaus::util::ArgInfo const arginfo(argc,argv);
		
		for ( uint64_t i = 0; i < arginfo.restargs.size(); ++i )
			if ( 
				arginfo.restargs[i] == "-v"
				||
				arginfo.restargs[i] == "--version"
			)
			{
				std::cerr << ::biobambam::Licensing::license();
				return EXIT_SUCCESS;
			}
			else if ( 
				arginfo.restargs[i] == "-h"
				||
				arginfo.restargs[i] == "--help"
			)
			{
				std::cerr << ::biobambam::Licensing::license() << std::endl;
				std::cerr << "Key=Value pairs:" << std::endl;
				std::cerr << std::endl;
				
				std::vector< std::pair<std::string,std::string> > V;
				
				V.push_back ( std::pair<std::string,std::string> ( "collate=<[1]>", "collate pairs" ) );
				V.push_back ( std::pair<std::string,std::string> ( "filename=<[stdin]>", "input filename (default: read file from standard input)" ) );
				#if defined(BIOBAMBAM_LIBMAUS_HAVE_IO_LIB)
				V.push_back ( std::pair<std::string,std::string> ( "inputformat=<[bam]>", "input format: cram, bam or sam" ) );
				#else
				V.push_back ( std::pair<std::string,std::string> ( "inputformat=<[bam]>", "input format: bam" ) );
				#endif
				V.push_back ( std::pair<std::string,std::string> ( "exclude=<[SECONDARY]>", "exclude alignments matching any of the given flags" ) );
				V.push_back ( std::pair<std::string,std::string> ( "disablevalidation=<[0]>", "disable validation of input data" ) );
				V.push_back ( std::pair<std::string,std::string> ( "colhlog=<[18]>", "base 2 logarithm of hash table size used for collation" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("colsbs=<[")+libmaus::util::NumberSerialisation::formatNumber(128ull*1024*1024,0)+"]>", "size of hash table overflow list in bytes" ) );
				V.push_back ( std::pair<std::string,std::string> ( std::string("T=<[") + arginfo.getDefaultTmpFileName() + "]>" , "temporary file name" ) );
				
				::biobambam::Licensing::printMap(std::cerr,V);

				std::cerr << std::endl;
				std::cerr << "Alignment flags: PAIRED,PROPER_PAIR,UNMAP,MUNMAP,REVERSE,MREVERSE,READ1,READ2,SECONDARY,QCFAIL,DUP" << std::endl;

				std::cerr << std::endl;
				return EXIT_SUCCESS;
			}
		
			
		bamcollate2(arginfo);
		
		std::cerr << "[V] " << libmaus::util::MemUsage() << " wall clock time " << rtc.formatTime(rtc.getElapsedSeconds()) << std::endl;		
	}
	catch(std::exception const & ex)
	{
		std::cerr << ex.what() << std::endl;
		return EXIT_FAILURE;
	}
}