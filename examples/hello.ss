// ======================================
// SSharp Comprehensive Stress Test
// ======================================

// 1. FUNCTIONS

int Add(int a, int b)
{
    return a + b;
}

int Subtract(int a, int b)
{
    return a - b;
}

int Multiply(int a, int b)
{
    return a * b;
}

int SumSix(int a, int b, int c, int d, int e, int f)
{
    return a + b + c + d + e + f;
}

int Square(int x)
{
    return x * x;
}

// 2. VARIABLES

int x = 10;
int y = 20;
int negative = -15;

bool enabled = true;
bool disabled = false;

string message = "SSharp is running!";

Console.WriteLine("========== SSharp STRESS TEST ==========");

// 3. BASIC MATHEMATICS

Console.WriteLine("--- Mathematics ---");

Console.WriteLine(Add(10, 20));          // 30
Console.WriteLine(Subtract(20, 10));     // 10
Console.WriteLine(Multiply(10, 20));     // 200

Console.WriteLine(negative);             // -15
Console.WriteLine(negative + 5);         // -10
Console.WriteLine(negative * 2);         // -30

// 4. OPERATOR PRECEDENCE

Console.WriteLine("--- Operator Precedence ---");

Console.WriteLine(2 + 3 * 4);            // 14
Console.WriteLine((2 + 3) * 4);          // 20
Console.WriteLine(100 - 10 * 5);         // 50

// 5. FUNCTION ARGUMENTS

Console.WriteLine("--- Function Arguments ---");

Console.WriteLine(SumSix(1, 2, 3, 4, 5, 6)); // 21
Console.WriteLine(Square(9));                // 81

// 6. COMPARISONS

Console.WriteLine("--- Comparisons ---");

Console.WriteLine(10 == 10);              // true
Console.WriteLine(10 != 20);              // true
Console.WriteLine(10 < 20);               // true
Console.WriteLine(20 > 10);               // true
Console.WriteLine(10 <= 10);              // true
Console.WriteLine(20 >= 20);              // true

// 7. BOOLEAN LOGIC

Console.WriteLine("--- Boolean Logic ---");

Console.WriteLine(true && true);          // true
Console.WriteLine(true && false);         // false
Console.WriteLine(true || false);         // true
Console.WriteLine(false || false);        // false
Console.WriteLine(!false);                // true

// 8. IF / ELSE

Console.WriteLine("--- Conditions ---");

if (x < y)
{
    Console.WriteLine("X is smaller");
}
else
{
    Console.WriteLine("X is not smaller");
}

if (enabled)
{
    Console.WriteLine("Enabled");
}

if (disabled)
{
    Console.WriteLine("ERROR: Disabled should be false");
}
else
{
    Console.WriteLine("Disabled correctly");
}

// 9. FOR LOOP

Console.WriteLine("--- For Loop ---");

for (int i = 0; i < 10; i = i + 1)
{
    Console.WriteLine(i);
}

// 10. WHILE LOOP

Console.WriteLine("--- While Loop ---");

int counter = 0;

while (counter < 5)
{
    Console.WriteLine(counter);
    counter = counter + 1;
}

// 11. NESTED LOOPS

Console.WriteLine("--- Nested Loops ---");

for (int i = 0; i < 3; i = i + 1)
{
    for (int j = 0; j < 3; j = j + 1)
    {
        Console.WriteLine(i * 10 + j);
    }
}

// 12. STRING OPERATIONS

Console.WriteLine("--- Strings ---");

Console.WriteLine(message);
Console.WriteLine("Hello");
Console.WriteLine("SSharp");

// 13. VARIABLE ASSIGNMENT

Console.WriteLine("--- Assignments ---");

int number = 10;

Console.WriteLine(number);

number = 50;

Console.WriteLine(number);

number = number + 25;

Console.WriteLine(number);

// 14. SHORT CIRCUIT LOGIC

Console.WriteLine("--- Short Circuit ---");

Console.WriteLine(false && (10 > 5));
Console.WriteLine(true || (10 < 5));

// 15. FINAL

Console.WriteLine("========== ALL TESTS FINISHED ==========");