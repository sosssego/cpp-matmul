#include <fstream>
#include <cstdint>
#include <random>
#include <functional>
#include <iostream>
#include <chrono>

typedef struct Mat
{
	unsigned width;
	unsigned height;
	unsigned rowSpan;
	float *mat;
} Mat;

template<typename Rand>
static void RandInitMat(Mat *m, Rand &r)
{
	for(unsigned y=0; y<m->height; ++y)
		for(unsigned x=0; x<m->width; ++x)
			m->mat[y*m->rowSpan + x] = r();
}

const Mat LoadMat(const char * const filename) {
    Mat mat;
    uint32_t matSize;

    std::ifstream in(filename, std::ios::binary | std::ios::in);

    if (!in.is_open()) {
        std::cerr << "Err loading!\n";
        return {};
    }

    in.read((char*)&mat, 3 * sizeof(uint32_t));
    in.read((char*)&matSize, sizeof(uint32_t));
    in.seekg(12*sizeof(uint32_t), std::ios::cur);
    mat.mat = (float*)malloc(matSize);
    in.read((char*)mat.mat, matSize);

    in.close();

    return mat;
}

static void DumpMat(const char *filename, const Mat &m)
{
	uint32_t header[16];
	std::ofstream out(filename, std::ofstream::binary | std::ofstream::out);

	header[0] = m.width;
	header[1] = m.height;
	header[2] = m.rowSpan;
	header[3] = m.height * m.rowSpan * sizeof(float);

	out.write(reinterpret_cast<const char*>(header), sizeof(header));
	out.write(reinterpret_cast<const char*>(m.mat), header[3]);

	out.close();
}

static unsigned RoundUpPwr2(unsigned val, unsigned pwr2)
{
	return (val + (pwr2 - 1)) & (~(pwr2 - 1));
}

static void PrintMat(const Mat &mat) {
    for (int i = 0; i < mat.height; i++) {
        for (int j = 0; j < mat.width; ++j) {
            printf("%f ", mat.mat[i*mat.rowSpan + j]);
        }
        printf("\n");
    }
}

/* Single threaded, do i need to multithread this as well? 
Honestly, I don't think it will have any significant effect. n^2 vs n^3 */
const Mat TransposeMat(const Mat &mat) {
    const unsigned tRowSpan = RoundUpPwr2(mat.height, 64 / sizeof(float));
    float * const tData = (float*)malloc(mat.width*tRowSpan * sizeof(float));

    Mat T{
        mat.height,
        mat.width,
        tRowSpan,
        tData
    };

    // hah, the loops are truly interchangable as we encounter a cache miss either ways
    for (int rowT = 0; rowT < T.height; ++rowT) {
        for (int colT = 0; colT < T.width; ++colT) {
            tData[rowT*tRowSpan + colT] = mat.mat[colT*mat.rowSpan + rowT];
        }
    }

    return T;
}

const Mat ST_TransposedBMatMul(const Mat& matA, const Mat& matB) {
    /*
    * Now, I thought transposing B and then traversing it row order would help and it does!
    * Also, note that, if we manually unrolled the loop here, compiler wouldn't vectorize the loop for some reason
    * (1301: Loop stride is not +1.) is the exact compiler message.
    */
    float * __restrict const matData = (float*)malloc(matA.height * matB.rowSpan * sizeof(float));

    Mat matC{
        matB.width,
        matA.height,
        matB.rowSpan,
        matData
    };

    matC.rowSpan = matB.width;
    const Mat matBT = TransposeMat(matB);
    for (int rowC = 0; rowC < matA.height; ++rowC) {
        //if (rowC % 10 == 0)
        //    printf("row: %d of %d\n", rowC, matA.height);
        for (int colC = 0; colC < matB.width; ++colC) {
            float accumulate = 0;
            for (int pos = 0; pos < matA.width; ++pos) {
                accumulate += matA.mat[rowC*matA.rowSpan + pos] * matBT.mat[colC*matBT.rowSpan + pos];
            }
            matData[rowC*matB.width + colC] = accumulate;
        }
    }

    return matC;
}

int _cdecl main(int argc, char *argv[])
{
	static const unsigned ALIGN = 64;
	static const unsigned FLT_ALIGN = ALIGN / sizeof(float);

	std::random_device rd;
	//std::uniform_int_distribution<unsigned> matSizeDist(10, 100); //small
	std::uniform_int_distribution<unsigned> matSizeDist(8192, 8192); //big
	std::uniform_real_distribution<float> matValDist(-50.0f, 50.0f);
	auto matRand = std::bind(matValDist, std::ref(rd));
	auto sizeRand = std::bind(matSizeDist, std::ref(rd));
	Mat a, b;

	a.width = sizeRand();
	a.height = sizeRand();
	a.rowSpan = RoundUpPwr2(a.width, FLT_ALIGN);

	b.width = sizeRand();
	b.height = a.width;
	b.rowSpan = RoundUpPwr2(b.width, FLT_ALIGN);

	/*c.height = a.height;
	c.width = b.width;
	c.rowSpan = RoundUpPwr2(c.width, FLT_ALIGN);*/

	a.mat = new float[a.rowSpan*a.height];
	b.mat = new float[b.rowSpan*b.height];
	//c.mat = new float[c.rowSpan*c.height];
	
	RandInitMat(&a, matRand);
	RandInitMat(&b, matRand);

    printf("a: [%d %d] | b: [%d %d]\n", a.width, a.height, b.width, b.height);

	/* Generate valid output through transposed multiplication,
    Changed this to generate valid output for big matrices quicker.
    I checked it with naive soln. and it works. */

    auto start = std::chrono::high_resolution_clock::now();

    const Mat c = ST_TransposedBMatMul(a, b);

    auto end = std::chrono::high_resolution_clock::now();

    std::cout << "Generation w/ tranposed mult. took: " 
        << std::chrono::duration_cast<std::chrono::microseconds>(end - start).count() 
        << " microseconds.\n";

	DumpMat("matrixA.bin", a);
	DumpMat("matrixB.bin", b);
	DumpMat("matrixAB.bin", c);

	delete[] a.mat;
	delete[] b.mat;
	delete[] c.mat;

	return 0;
}
